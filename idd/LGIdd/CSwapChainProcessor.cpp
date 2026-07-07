/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "CSwapChainProcessor.h"

#include <avrt.h>
#include <smmintrin.h>
#include "CDebug.h"
#include "CPipeServer.h"
#include "CPlatformInfo.h"

CSwapChainProcessor::CSwapChainProcessor(IDDCX_MONITOR monitor, CIndirectDeviceContext* devContext, IDDCX_SWAPCHAIN hSwapChain,
    LUID renderAdapter, HANDLE newFrameEvent) :
  m_monitor(monitor),
  m_devContext(devContext),
  m_hSwapChain(hSwapChain),
  m_renderAdapter(renderAdapter),
  m_newFrameEvent(newFrameEvent)
{
  /* the D3D devices, pools and LGMP are brought up by InitDevices() on the
   * processing thread — this constructor runs inside the AssignSwapChain
   * callback and must not block */

  /* MANUAL-RESET: termination is a latch observed by BOTH processing threads
   * and by mid-bring-up checks; an auto-reset event only wakes one waiter */
  m_terminateEvent.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));

  m_cursorDataEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_shapeBuffer = new BYTE[512 * 512 * 4];
}

CSwapChainProcessor::~CSwapChainProcessor()
{
  /* Runs when the LAST shared_ptr drops: every processing thread has already
   * exited (each held a reference) AND the monitor context released its
   * reference (Stop() + reset, or Start() failure). There is deliberately NO
   * join and NO wait here — this can run inside an IddCx DDI callback, and
   * IddCx's watchdog terminates WUDFHost when a callback overruns.
   *
   * IDDCX_SWAPCHAIN ownership: the driver NEVER deletes the swapchain object
   * at teardown. Per the IddCx object model ("IddCx Objects", WDK docs) the
   * OS creates and destroys IDDCX_SWAPCHAIN objects, and the swapchain is a
   * CHILD of the IDDCX_MONITOR — IddCxMonitorDeparture (our replug
   * primitive) destroys the monitor and with it the child swapchain. The
   * previous dtor-time WdfObjectDelete here was a double delete of that
   * already-destroyed child: the freed FxObject's lock faulted inside
   * FxObject::DeleteObject → unhandled exception → WUDFHost terminated →
   * devnode FAILED_POST_START (dump WUDFHost.exe-(PID-11016) 2026-07-03
   * 09:40Z, thread 9: ~CSwapChainProcessor → imp_WdfObjectDelete →
   * RtlEnterCriticalSection → KiUserExceptionDispatch). The only
   * doc-sanctioned driver-side delete is the mid-frame-processing "release
   * ownership so the OS re-offers" signal, which this driver does not use —
   * thread-exit errors escalate to the monitor replug primitive instead,
   * and departure/re-arrival produces the new offer. */
  m_hSwapChain = nullptr;

  if (m_dx12Device)
  {
    m_postProcessor.Reset();
    m_resPool.Reset();
    m_fbPool.Reset();
  }
  delete[] m_shapeBuffer;

  /* Release the D3D devices HERE (not via member destruction after this
   * body) so the runtime-lock-heavy device release still happens under the
   * processor-serial gate, then let the successor proceed. */
  m_dx11Device.reset();
  m_dx12Device.reset();
  if (m_serialHeld)
  {
    ReleaseSemaphore(m_devContext->ProcessorSerial(), 1, nullptr);
    m_serialHeld = false;
  }
}

void CSwapChainProcessor::Stop()
{
  /* signal-only; see the class comment. The threads observe the manual-reset
   * latch (every loop iteration polls it, including between bring-up steps)
   * and exit when their current — possibly tens-of-seconds — system call
   * returns. */
  SetEvent(m_terminateEvent.Get());
}

DWORD CALLBACK CSwapChainProcessor::_SwapChainThread(LPVOID arg)
{
  auto* self = reinterpret_cast<std::shared_ptr<CSwapChainProcessor>*>(arg);
  (*self)->SwapChainThread();
  /* releasing this reference may destroy the processor right here */
  delete self;
  return 0;
}

HRESULT CSwapChainProcessor::Start()
{
  /* IddCxSwapChainSetDevice runs on the PROCESSING THREAD, never here.
   * This method executes inside EvtIddCxMonitorAssignSwapChain, and a single
   * SetDevice call was observed blocking 22+ seconds during boot pairing
   * churn (2026-07-03 boots #2/#4) — holding the assign callback open that
   * long is what let the framework's post-start deadline and the replug
   * paths collide with a live callback (WUDFHost FAST_FAIL). Microsoft's
   * IddCx sample binds the device on the processing thread for the same
   * reason.
   *
   * The born-abandoned first swapchain of a boot (SetDevice fails
   * DXGI_ERROR_ACCESS_LOST forever) is now handled by the thread's own
   * SetDevice failure path: the thread self-exits and queues a monitor
   * replug — the single recovery primitive — instead of returning
   * ABANDON_SWAPCHAIN from the callback. Boot #3 (2026-07-03) demonstrated
   * the replug loop self-converging to a fed steady-state binding. */
  auto* self = new std::shared_ptr<CSwapChainProcessor>(shared_from_this());
  m_thread[0].Attach(CreateThread(nullptr, 0, _SwapChainThread, self, 0, nullptr));
  if (!m_thread[0].Get())
  {
    delete self;
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}

void CSwapChainProcessor::SwapChainThread()
{
  DWORD  avTask       = 0;
  HANDLE avTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);

  DEBUG_INFO("Start Thread");

  /* Serialize processor lifecycles: wait for the superseded processor to
   * finish tearing down (including its D3D device release) before this one
   * touches the d3d11 runtime. Both drive the SAME runtime instance, whose
   * internal lock otherwise convoys them — the successor's slow device
   * create blocks the predecessor's IddCxSwapChainFinishedProcessingFrame,
   * the acquired frame outlives IddCx's deadline, and IddCx terminates
   * WUDFHost (two dump-verified kills, 2026-07-04). Blocking is legal HERE
   * (own thread, not a DDI callback). Bounded: a predecessor stuck in a
   * tens-of-seconds syscall must not wedge recovery forever — after the
   * timeout we proceed unserialized (the pre-fix behaviour) and say so. */
  {
    HANDLE serialWait[] = { m_devContext->ProcessorSerial(), m_terminateEvent.Get() };
    const DWORD sw = WaitForMultipleObjects(ARRAYSIZE(serialWait), serialWait, FALSE, 15000);
    if (sw == WAIT_OBJECT_0)
      m_serialHeld = true;
    else if (sw == WAIT_OBJECT_0 + 1)
    {
      DEBUG_INFO("Swapchain thread stopped while waiting for predecessor teardown");
      AvRevertMmThreadCharacteristics(avTaskHandle);
      return;
    }
    else
      DEBUG_WARN("Predecessor processor teardown did not finish within 15s; "
                 "proceeding unserialized (runtime-lock convoy risk)");
  }

  const bool terminated = SwapChainThreadCore();

  /* the swapchain object is deleted by the DESTRUCTOR, not here: at this
   * point IddCx may still consider it assigned (self-exit on bring-up
   * failure), and deleting an assigned swapchain corrupts IddCx state */

  AvRevertMmThreadCharacteristics(avTaskHandle);

  /* the loop died on its own (device set failure, acquire failure, cursor
   * setup failure, cursor-thread topology loss, ...) with no unassign from
   * the OS — without escalation the acquire loop is simply gone and the
   * display sits frameless forever. Queue a monitor replug (picked up by
   * the LGMP timer / watchdog). */
  if (!terminated || InterlockedCompareExchange(&m_selfTerminated, 0, 0))
    m_devContext->NotifySwapChainSelfExit();
}

bool CSwapChainProcessor::InitDevices()
{
  /* runs on the processing thread; see the declaration comment. Failure
   * returns false -> the thread self-exits -> NotifySwapChainSelfExit queues
   * a monitor replug (the single recovery primitive). */
  m_forceFullDamage = g_settings.ReadBoolValue(L"HeliosForceFullDamage", false);
  if (m_forceFullDamage)
    DEBUG_INFO("HeliosForceFullDamage=1: dirty rects ignored, full-frame copy+damage every frame");

  unsigned reInitAttempts = 0;
reInit:
  if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return false;

  /* the staging texture belongs to the D3D11 device built below — never carry
   * one across a device (re)build (the RETRY arm re-enters here) */
  m_d11Staging.Reset();
  m_d11StagingDesc = {};

  m_dx11Device = std::make_shared<CD3D11Device>(m_renderAdapter);
  HRESULT dx11hr = m_dx11Device->Init();
  if (FAILED(dx11hr))
  {
    DEBUG_ERROR_HR(dx11hr, "InitDevices: CD3D11Device::Init FAILED (this gates the whole capture path)");
    m_dx11Device.reset();
    return false;
  }
  DEBUG_INFO("InitDevices: CD3D11Device::Init OK");

  /* D3D11CreateDevice can block for many seconds during pairing churn; a
   * Stop() may have arrived meanwhile — bail before further bring-up */
  if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return false;

  UINT64 alignSize = CPlatformInfo::GetPageSize();
  m_dx12Device = std::make_shared<CD3D12Device>(m_renderAdapter);
  switch (m_dx12Device->Init(m_devContext->GetIVSHMEM(), alignSize))
  {
    case CD3D12Device::SUCCESS:
      DEBUG_INFO("InitDevices: CD3D12Device::Init OK");
      m_resPool.Init(m_dx11Device, m_dx12Device);
      m_fbPool.Init(this);
      if (!m_postProcessor.Init(m_dx12Device))
        DEBUG_ERROR("Failed to initialize post processor");
      break;

    case CD3D12Device::FAILURE:
      DEBUG_WARN("InitDevices: CD3D12Device::Init FAILURE; using D3D11 readback path");
      m_dx12Device.reset();
      alignSize = CPlatformInfo::GetPageSize();
      break;

    case CD3D12Device::RETRY:
      DEBUG_INFO("InitDevices: CD3D12Device::Init RETRY");
      m_dx12Device.reset();
      m_dx11Device.reset();
      if (++reInitAttempts >= 3)
      {
        DEBUG_ERROR("InitDevices: CD3D12Device::Init RETRY %u times, giving up "
                    "(self-exit escalates to a monitor replug)", reInitAttempts);
        return false;
      }
      goto reInit;
  }

  if (!m_devContext->SetupLGMP(alignSize))
  {
    DEBUG_ERROR("SetupLGMP failed");
    return false;
  }

  return true;
}

HRESULT CSwapChainProcessor::SetSwapChainDevice()
{
  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = m_dx11Device->GetBaseDevice().As(&dxgiDevice);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to get the dxgiDevice");
    return hr;
  }

  if (m_dx12Device && IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority))
  {
    DEBUG_INFO("Using IddCxSetRealtimeGPUPriority");
    IDARG_IN_SETREALTIMEGPUPRIORITY arg = {0};
    arg.pDevice = dxgiDevice.Get();
    hr = IddCxSetRealtimeGPUPriority(m_hSwapChain, &arg);
    if (FAILED(hr))
      DEBUG_ERROR_HR(hr, "Failed to set realtime GPU thread priority");
  }
  else
  {
    DEBUG_INFO("Using SetGPUThreadPriority");
    dxgiDevice->SetGPUThreadPriority(7);
  }

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();

  /* while inside IddCxSwapChainSetDevice no replug may depart the monitor
   * (iddcx object teardown under a live call = list corruption failfast);
   * NotifyBindingDone also restarts the stale-binding clock on success */
  m_devContext->NotifyBindingStart();
  for (UINT attempt = 0; attempt < 5; ++attempt)
  {
    hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
    if (SUCCEEDED(hr))
      break;

    DEBUG_WARN_HR(hr, "IddCxSwapChainSetDevice attempt %u failed", attempt + 1);
    if (hr != DXGI_ERROR_ACCESS_LOST)
      break;

    Sleep(100);
  }
  m_devContext->NotifyBindingDone();
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "IddCxSwapChainSetDevice Failed");
    return hr;
  }

  m_swapChainDeviceSet = true;
  return S_OK;
}

bool CSwapChainProcessor::SwapChainThreadCore()
{
  HRESULT hr = S_OK;

  if (!InitDevices())
  {
    /* a Stop() during bring-up is a NORMAL termination, not a self-exit —
     * escalating it would queue a replug against whatever new binding
     * superseded this one */
    if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
      return true;
    DEBUG_ERROR("device bring-up failed on the swapchain thread; "
                "exiting so the monitor lifecycle can replug");
    return false;
  }

  if (!m_swapChainDeviceSet)
  {
    hr = SetSwapChainDevice();
    if (FAILED(hr))
    {
      if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
        return true;
      /* an acquire loop with no bound device spins framelessly forever —
       * exit instead; the self-exit path escalates to a monitor replug */
      DEBUG_ERROR_HR(hr, "SetSwapChainDevice failing on the swapchain thread; "
                         "exiting so the monitor lifecycle can replug");
      return false;
    }
  }

  /* device bring-up complete: only from here may the first-frame watchdog
   * count zero acquires against this binding */
  m_devContext->NotifyDevicesReady();

  BOOL swapChainInSystemMemory = FALSE;
  if (!m_dx12Device)
  {
    hr = IddCxSwapChainInSystemMemory(m_hSwapChain, &swapChainInSystemMemory);
    if (FAILED(hr))
    {
      DEBUG_WARN_HR(hr, "IddCxSwapChainInSystemMemory failed; using D3D11 staging path");
      swapChainInSystemMemory = FALSE;
    }
    DEBUG_INFO("IddCx swapchain in system memory: %u", swapChainInSystemMemory);
    DEBUG_INFO("Using %s swapchain path", swapChainInSystemMemory ? "IddCx system-buffer" : "D3D11 staging");
  }

  IDARG_IN_SETUP_HWCURSOR c = {};
  c.CursorInfo.Size                  = sizeof(c.CursorInfo);
  c.CursorInfo.AlphaCursorSupport    = TRUE;
  c.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
  c.CursorInfo.MaxX                  = 512;
  c.CursorInfo.MaxY                  = 512;
  c.hNewCursorDataAvailable          = m_cursorDataEvent.Get();
  NTSTATUS status = IddCxMonitorSetupHardwareCursor(m_monitor, &c);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR("IddCxMonitorSetupHardwareCursor Failed (0x%08x)", status);
    return false;
  }

  m_lastShapeId = 0;
  {
    auto* cself = new std::shared_ptr<CSwapChainProcessor>(shared_from_this());
    m_thread[1].Attach(CreateThread(nullptr, 0, _CursorThread, cself, 0, nullptr));
    if (!m_thread[1].Get())
      delete cself;
  }

  // postpone sending this to ensure we dont spam messages if we end up in a
  // restart loop while waiting for a valid configuration
  g_pipe.SetGPUStatus(m_dx11Device->IsSoftware());

  UINT lastFrameNumber = 0;
  UINT pendingPolls = 0;
  bool terminated = false;
  for (;;)
  {
    if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    {
      terminated = true;
      break;
    }

    UINT frameNumber = 0;
    UINT dirtyRectCount = 0;
    UINT moveRegionCount = 0;
    ComPtr<IDXGIResource> surface;
    IDDCX_SYSTEM_BUFFER_INFO sysBuffer = {};
    bool useSystemBuffer = false;

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
    if (!m_dx12Device)
    {
      if (swapChainInSystemMemory)
      {
        IDDCX_METADATA meta = {};
        meta.Size = sizeof(meta);
        sysBuffer.Size = sizeof(sysBuffer);

        IDARG_OUT_RELEASEANDACQUIRESYSTEMBUFFER buffer = {};
        buffer.pMetaData = &meta;
        buffer.pBufferInfo = &sysBuffer;

        hr = IddCxSwapChainReleaseAndAcquireSystemBuffer(m_hSwapChain, &buffer);
        if (hr != E_PENDING)
          DEBUG_INFO("IddCx system acquire returned hr=0x%08x frame=%u dirty=%u buffer=%p pitch=%u",
            hr, meta.PresentationFrameNumber, meta.DirtyRectCount,
            sysBuffer.pBuffer, sysBuffer.Pitch);
        if (SUCCEEDED(hr))
        {
          frameNumber = meta.PresentationFrameNumber;
          dirtyRectCount = meta.DirtyRectCount;
          moveRegionCount = meta.MoveRegionCount;
          useSystemBuffer = true;
        }
      }
      else
      {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};

        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);
        if (hr != E_PENDING)
          DEBUG_INFO("IddCx acquire returned hr=0x%08x frame=%u dirty=%u moves=%u surface=%p",
            hr, buffer.MetaData.PresentationFrameNumber,
            buffer.MetaData.DirtyRectCount, buffer.MetaData.MoveRegionCount,
            buffer.MetaData.pSurface);
        if (SUCCEEDED(hr))
        {
          frameNumber = buffer.MetaData.PresentationFrameNumber;
          dirtyRectCount = buffer.MetaData.DirtyRectCount;
          moveRegionCount = buffer.MetaData.MoveRegionCount;
          surface.Attach(buffer.MetaData.pSurface);
        }
      }
    }
    else if (m_devContext->CanProcessFP16())
    {
      IDARG_IN_RELEASEANDACQUIREBUFFER2 acquireIn = {};
      acquireIn.Size = sizeof(acquireIn);
      acquireIn.AcquireSystemMemoryBuffer = FALSE;

      IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer = {};
      buffer.MetaData.Size = sizeof(buffer.MetaData);

      hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &acquireIn, &buffer);
      if (hr != E_PENDING)
        DEBUG_INFO("IddCx acquire2 returned hr=0x%08x frame=%u dirty=%u surface=%p",
          hr, buffer.MetaData.PresentationFrameNumber,
          buffer.MetaData.DirtyRectCount, buffer.MetaData.pSurface);
      if (SUCCEEDED(hr))
      {
        frameNumber = buffer.MetaData.PresentationFrameNumber;
        dirtyRectCount = buffer.MetaData.DirtyRectCount;
        /* IDDCX_METADATA2 carries no MoveRegionCount — acquire2 frames
         * deliver all updates via dirty rects */
        surface.Attach(buffer.MetaData.pSurface);
      }
    }
    else
#endif
    {
      IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};

      hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);
      if (hr != E_PENDING)
        DEBUG_INFO("IddCx acquire legacy returned hr=0x%08x frame=%u dirty=%u moves=%u surface=%p",
          hr, buffer.MetaData.PresentationFrameNumber,
          buffer.MetaData.DirtyRectCount, buffer.MetaData.MoveRegionCount,
          buffer.MetaData.pSurface);
      if (SUCCEEDED(hr))
      {
        frameNumber = buffer.MetaData.PresentationFrameNumber;
        dirtyRectCount = buffer.MetaData.DirtyRectCount;
        moveRegionCount = buffer.MetaData.MoveRegionCount;
        surface.Attach(buffer.MetaData.pSurface);
      }
    }

    if (hr == E_PENDING)
    {
      if (!m_dx12Device && (++pendingPolls == 1 || (pendingPolls % 300) == 0))
        DEBUG_INFO("IddCx swapchain pending (polls=%u, systemMemory=%u, legacyAcquire=1)",
          pendingPolls, swapChainInSystemMemory);

      HANDLE waitHandles[] =
      {
        m_newFrameEvent,
        m_terminateEvent.Get()
      };
      DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 17);
      if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT)
        continue;
      else if (waitResult == WAIT_OBJECT_0 + 1)
      {
        terminated = true;
        break;
      }
      else
      {
        hr = HRESULT_FROM_WIN32(waitResult);
        break;
      }
    }
    else if (SUCCEEDED(hr))
    {
      pendingPolls = 0;
      /* disarm the first-frame (stale-binding) watchdog for this binding */
      m_devContext->NotifyFrameAcquired();
      /* gate ReplugMonitor while this frame is outstanding: a monitor
       * departure between acquire and FinishedProcessingFrame destroys the
       * child swapchain with the frame unreleased and IddCx kills the host
       * process (DriverDidNotReleaseFrame). Paired with NotifyFrameReleased
       * immediately after the Finish attempt on every path below. */
      m_devContext->NotifyFrameHeld();
      /* the D3D11 readback path previously finished the frame here
       * ("d3d11-acquired-immediate"), releasing the buffer at CopyResource
       * SUBMIT time — dwm could then re-render into it before the copy
       * executed on the host GPU, so the delivered content ran newer than
       * the dirty rects (persistent ghosts once motion stopped). The frame
       * is now finished by the post-processing block below, after the
       * staging Map has completed. Cycle time is unchanged: this thread
       * already blocked on the Map before the next acquire. */
      if (frameNumber != lastFrameNumber)
      {
        lastFrameNumber = frameNumber;
        /* HeliosForceFullDamage: a count above LG_MAX_DIRTY_RECTS takes each
         * frame path's existing overflow branch — SetFullDamage()/full rect —
         * so both the staging copy and the KVMFR damage rects cover the whole
         * frame (see the member comment in CSwapChainProcessor.h) */
        if (m_forceFullDamage)
        {
          dirtyRectCount = LG_MAX_DIRTY_RECTS + 1;
          moveRegionCount = 0;
        }
        if (useSystemBuffer)
          /* move regions unhandled on this path: degrade to full damage */
          SwapChainNewFrameSystem(sysBuffer,
            moveRegionCount ? LG_MAX_DIRTY_RECTS + 1 : dirtyRectCount);
        else if (!m_dx12Device)
        {
          DEBUG_TRACE("D3D11 fallback acquired frame=%u dirty=%u moves=%u surface=%p",
            frameNumber, dirtyRectCount, moveRegionCount, surface.Get());
          SwapChainNewFrameD3D11(surface, dirtyRectCount, moveRegionCount);
          surface.Reset();
        }
        else
          /* move regions unhandled on this path: degrade to full damage */
          SwapChainNewFrame(surface,
            moveRegionCount ? LG_MAX_DIRTY_RECTS + 1 : dirtyRectCount);
      }

      // report that all GPU processing for this frame has been queued
      // (on the D3D11 readback path this runs after the staging Map, so the
      // buffer is only handed back once its content has been captured)
      hr = FinishProcessingFrame("main-loop");
      m_devContext->NotifyFrameReleased();
      if (FAILED(hr))
      {
        if (hr == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
          m_devContext->OnSwapChainLost();
        break;
      }
    }
    else
    {
      if (!m_dx12Device)
      {
        const LONG failures = InterlockedIncrement(&m_systemAcquireFailures);
        if (failures == 1 || (failures % 60) == 0)
          DEBUG_WARN_HR(hr, "IddCx swapchain acquire failed (failures=%ld, systemMemory=%u, legacyAcquire=1)",
            failures, swapChainInSystemMemory);

      }

      if (hr == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
        m_devContext->OnSwapChainLost();
      else if (hr == DXGI_ERROR_ACCESS_LOST)
        m_devContext->OnSwapChainDeviceLost();
      break;
    }
  }

  return terminated;
}

HRESULT CSwapChainProcessor::FinishProcessingFrame(const char * reason)
{
  HRESULT hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
  if (FAILED(hr))
    DEBUG_ERROR_HR(hr, "IddCxSwapChainFinishedProcessingFrame Failed (%s)", reason);
  else
    DEBUG_TRACE("IddCxSwapChainFinishedProcessingFrame OK (%s)", reason);
  return hr;
}

void CSwapChainProcessor::CompletionFunction(
  CD3D12CommandQueue * queue, bool result, void * param1, void * param2)
{
  UNREFERENCED_PARAMETER(queue);

  auto sc    = (CSwapChainProcessor *)param1;
  auto fbRes = (CFrameBufferResource*)param2;

  // fail gracefully
  if (!result)
  {
    sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());
    return;
  }

  if (sc->m_dx12Device->IsIndirectCopy())
    sc->m_devContext->WriteFrameBuffer(
      fbRes->GetFrameIndex(),
      fbRes->GetMap(), 0, fbRes->GetFrameSize(), true);
  else
    sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());

  sc->m_devContext->PresentHeliosFrame(fbRes->GetFrameIndex());
}


/* Copy from a WRITE-COMBINED source mapping. Plain loads from WC memory are
 * uncacheable (~160-600 MB/s measured on the venus staging mapping, 2026-07-06
 * BW probe); MOVNTDQA streaming loads use the WC read buffers and recover
 * multiple GB/s. Falls back to memcpy for the unaligned head/tail. The source
 * must not be concurrently written (we hold the acquired frame). */
static void CopyFromWC(void * dst, const void * src, size_t bytes)
{
  uint8_t * d = static_cast<uint8_t *>(dst);
  const uint8_t * s = static_cast<const uint8_t *>(src);

  const size_t headBytes = (16 - (reinterpret_cast<uintptr_t>(s) & 15)) & 15;
  if (headBytes)
  {
    const size_t n = headBytes < bytes ? headBytes : bytes;
    memcpy(d, s, n);
    d += n; s += n; bytes -= n;
  }

  while (bytes >= 64)
  {
    const __m128i * sv = reinterpret_cast<const __m128i *>(s);
    const __m128i v0 = _mm_stream_load_si128(const_cast<__m128i *>(sv + 0));
    const __m128i v1 = _mm_stream_load_si128(const_cast<__m128i *>(sv + 1));
    const __m128i v2 = _mm_stream_load_si128(const_cast<__m128i *>(sv + 2));
    const __m128i v3 = _mm_stream_load_si128(const_cast<__m128i *>(sv + 3));
    __m128i * dv = reinterpret_cast<__m128i *>(d);
    _mm_storeu_si128(dv + 0, v0);
    _mm_storeu_si128(dv + 1, v1);
    _mm_storeu_si128(dv + 2, v2);
    _mm_storeu_si128(dv + 3, v3);
    s += 64; d += 64; bytes -= 64;
  }

  if (bytes)
    memcpy(d, s, bytes);
}

static bool IsFullDamage(const RECT * dirtyRects, unsigned nbDirtyRects,
  const D3D12_RESOURCE_DESC& desc)
{
  return nbDirtyRects == 0 ||
    (nbDirtyRects == 1 &&
      dirtyRects[0].left   == 0 &&
      dirtyRects[0].top    == 0 &&
      dirtyRects[0].right  == (LONG)desc.Width &&
      dirtyRects[0].bottom == (LONG)desc.Height);
}

static void CopyDirtyRect(ComPtr<ID3D12GraphicsCommandList> list,
  D3D12_TEXTURE_COPY_LOCATION * dstLoc,
  D3D12_TEXTURE_COPY_LOCATION * srcLoc,
  const RECT& rect)
{
  D3D12_BOX box = {};
  box.left   = rect.left;
  box.top    = rect.top;
  box.front  = 0;
  box.right  = rect.right;
  box.bottom = rect.bottom;
  box.back   = 1;

  list->CopyTextureRegion(dstLoc, box.left, box.top, 0, srcLoc, &box);
}

static bool ClipDirtyRect(RECT& rect, const D3D12_RESOURCE_DESC& desc)
{
  const LONG maxRight  = (LONG)desc.Width;
  const LONG maxBottom = (LONG)desc.Height;

  if (rect.left   < 0        ) rect.left   = 0;
  if (rect.top    < 0        ) rect.top    = 0;
  if (rect.right  > maxRight ) rect.right  = maxRight;
  if (rect.bottom > maxBottom) rect.bottom = maxBottom;

  return rect.left < rect.right && rect.top < rect.bottom;
}

static void ClipDirtyRects(RECT dirtyRects[], unsigned * nbDirtyRects,
  const D3D12_RESOURCE_DESC& desc)
{
  unsigned out = 0;
  for (unsigned i = 0; i < *nbDirtyRects; ++i)
  {
    RECT rect = dirtyRects[i];
    if (ClipDirtyRect(rect, desc))
      dirtyRects[out++] = rect;
  }
  *nbDirtyRects = out;
}

static FrameType GetFrameType(DXGI_FORMAT format)
{
  switch (format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM    : return FRAME_TYPE_BGRA;
    case DXGI_FORMAT_R8G8B8A8_UNORM    : return FRAME_TYPE_RGBA;
    case DXGI_FORMAT_R10G10B10A2_UNORM : return FRAME_TYPE_RGBA10;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FRAME_TYPE_RGBA16F;
    default                            : return FRAME_TYPE_INVALID;
  }
}

bool CSwapChainProcessor::SwapChainNewFrameSystem(const IDDCX_SYSTEM_BUFFER_INFO& sysBuffer, unsigned dirtyRectCount)
{
  if (!sysBuffer.pBuffer || sysBuffer.Pitch == 0 || sysBuffer.Width == 0 || sysBuffer.Height == 0)
  {
    DEBUG_ERROR("Invalid IddCx system buffer");
    return false;
  }

  FrameType frameType = GetFrameType(sysBuffer.Format);
  if (frameType == FRAME_TYPE_INVALID)
  {
    DEBUG_ERROR("Unsupported system-buffer frame format 0x%x", sysBuffer.Format);
    return false;
  }

  D12FrameFormat frameFormat = {};
  frameFormat.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  frameFormat.desc.Width = sysBuffer.Width;
  frameFormat.desc.Height = sysBuffer.Height;
  frameFormat.desc.DepthOrArraySize = 1;
  frameFormat.desc.MipLevels = 1;
  frameFormat.desc.Format = sysBuffer.Format;
  frameFormat.desc.SampleDesc.Count = 1;
  frameFormat.width = sysBuffer.Width;
  frameFormat.height = sysBuffer.Height;
  frameFormat.format = frameType;
  frameFormat.hdr = sysBuffer.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  frameFormat.hdrPQ = false;

  RECT dirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbDirtyRects = dirtyRectCount;
  if (nbDirtyRects == 0 || nbDirtyRects > ARRAYSIZE(dirtyRects) || m_nbDirtyRects == 0)
  {
    dirtyRects[0].left = 0;
    dirtyRects[0].top = 0;
    dirtyRects[0].right = static_cast<LONG>(sysBuffer.Width);
    dirtyRects[0].bottom = static_cast<LONG>(sysBuffer.Height);
    nbDirtyRects = 1;
  }
  else
  {
    IDARG_IN_GETDIRTYRECTS dirtyIn = {};
    dirtyIn.DirtyRectInCount = nbDirtyRects;
    dirtyIn.pDirtyRects = dirtyRects;

    IDARG_OUT_GETDIRTYRECTS dirtyOut = {};
    HRESULT hr = IddCxSwapChainGetDirtyRects(m_hSwapChain, &dirtyIn, &dirtyOut);
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "IddCxSwapChainGetDirtyRects Failed");
      dirtyRects[0].left = 0;
      dirtyRects[0].top = 0;
      dirtyRects[0].right = static_cast<LONG>(sysBuffer.Width);
      dirtyRects[0].bottom = static_cast<LONG>(sysBuffer.Height);
      nbDirtyRects = 1;
    }
    else
      nbDirtyRects = dirtyOut.DirtyRectOutCount;
  }
  ClipDirtyRects(dirtyRects, &nbDirtyRects, frameFormat.desc);

  auto buffer = m_devContext->PrepareFrameBuffer(sysBuffer.Pitch, frameFormat, frameFormat, dirtyRects, nbDirtyRects);
  if (!buffer.mem)
    return false;

  const size_t bpp = frameType == FRAME_TYPE_RGBA16F ? 8 : 4;
  const size_t srcRowBytes = (size_t)sysBuffer.Width * bpp;
  const size_t rowBytes = srcRowBytes < sysBuffer.Pitch ? srcRowBytes : sysBuffer.Pitch;
  size_t rows = sysBuffer.Height < buffer.height ? sysBuffer.Height : buffer.height;

  const uint8_t * src = static_cast<const uint8_t *>(sysBuffer.pBuffer);
  uint8_t * dst = buffer.mem;
  for (size_t row = 0; row < rows; ++row)
  {
    const size_t copyBytes = rowBytes < buffer.pitch ? rowBytes : buffer.pitch;
    memcpy(dst + row * buffer.pitch, src + row * sysBuffer.Pitch, copyBytes);
    if (buffer.pitch > copyBytes)
      memset(dst + row * buffer.pitch + copyBytes, 0, buffer.pitch - copyBytes);
  }

  m_devContext->FinalizeFrameBuffer(buffer.frameIndex, rows * buffer.pitch);
  m_devContext->PresentHeliosFrame(buffer.frameIndex);
  memcpy(m_dirtyRects, dirtyRects, nbDirtyRects * sizeof(*m_dirtyRects));
  m_nbDirtyRects = nbDirtyRects;

  const LONG frames = InterlockedIncrement(&m_systemFrameCount);
  if (frames == 1 || (frames % 120) == 0)
    DEBUG_INFO("System-buffer frame %ld: %ux%u srcPitch=%u dstPitch=%u format=0x%x dirty=%u rows=%llu rowBytes=%llu",
      frames, sysBuffer.Width, sysBuffer.Height, sysBuffer.Pitch, buffer.pitch,
      sysBuffer.Format, nbDirtyRects, (unsigned long long)rows, (unsigned long long)rowBytes);

  return true;
}

void CSwapChainProcessor::AccumulatePendingDamage(const RECT * rects, unsigned count)
{
  if (m_pendingFullDamage)
    return;

  if (m_nbPendingDamage + count > ARRAYSIZE(m_pendingDamage))
  {
    m_pendingFullDamage = true;
    m_nbPendingDamage   = 0;
    return;
  }

  memcpy(m_pendingDamage + m_nbPendingDamage, rects, count * sizeof(*rects));
  m_nbPendingDamage += count;
}

bool CSwapChainProcessor::SwapChainNewFrameD3D11(ComPtr<IDXGIResource> acquiredBuffer,
  unsigned dirtyRectCount, unsigned moveRegionCount)
{
  /* the CALLER owns FinishProcessingFrame and issues it after this returns —
   * the acquired buffer must stay unreleased until the staging Map completes
   * (see the declaration comment); every early return below therefore just
   * returns and lets the main loop finish the frame. Every return AFTER the
   * dirty rects were consumed from IddCx must first bank them as pending
   * damage (see the member comment); a pre-rects failure banks FULL damage
   * because the frame's rects are unknown but IddCx still counts them as
   * handled once the caller finishes the frame. */

  LARGE_INTEGER qpcFreq;
  QueryPerformanceFrequency(&qpcFreq);
  auto nowUs = [&]() -> LONGLONG
  {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000ll / qpcFreq.QuadPart;
  };
  const LONGLONG tEnter = nowUs();

  if (!acquiredBuffer)
  {
    DEBUG_ERROR("D3D11 fallback acquired a null buffer");
    m_pendingFullDamage = true;
    return false;
  }

  ComPtr<ID3D11Texture2D> texture;
  DEBUG_TRACE("D3D11 fallback: As(ID3D11Texture2D)");
  HRESULT hr = acquiredBuffer.As(&texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to obtain the ID3D11Texture2D from the acquiredBuffer");
    m_pendingFullDamage = true;
    return false;
  }

  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  DEBUG_INFO("D3D11 fallback frame: %ux%u format=0x%x sample=%u/%u dirty=%u",
    desc.Width, desc.Height, desc.Format, desc.SampleDesc.Count,
    desc.SampleDesc.Quality, dirtyRectCount);

  FrameType frameType = GetFrameType(desc.Format);
  if (frameType == FRAME_TYPE_INVALID)
  {
    DEBUG_ERROR("Unsupported D3D11 fallback frame format 0x%x", desc.Format);
    m_pendingFullDamage = true;
    return false;
  }

  D12FrameFormat frameFormat = {};
  frameFormat.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  frameFormat.desc.Width = desc.Width;
  frameFormat.desc.Height = desc.Height;
  frameFormat.desc.DepthOrArraySize = 1;
  frameFormat.desc.MipLevels = 1;
  frameFormat.desc.Format = desc.Format;
  frameFormat.desc.SampleDesc.Count = desc.SampleDesc.Count;
  frameFormat.desc.SampleDesc.Quality = desc.SampleDesc.Quality;
  frameFormat.width = desc.Width;
  frameFormat.height = desc.Height;
  frameFormat.format = frameType;
  frameFormat.hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  frameFormat.hdrPQ = false;

  const bool stagingFormatChanged =
    !m_d11Staging ||
    m_d11StagingDesc.Width  != desc.Width  ||
    m_d11StagingDesc.Height != desc.Height ||
    m_d11StagingDesc.Format != desc.Format;

  RECT dirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbDirtyRects = 0;
  /* full damage when: nothing was reported at all (unknowable), either
   * count overflows our capacity, or no frame has been delivered yet
   * (the client has no baseline for this stream) */
  bool fullDamage =
    (dirtyRectCount == 0 && moveRegionCount == 0) ||
    dirtyRectCount  > ARRAYSIZE(dirtyRects)       ||
    moveRegionCount > ARRAYSIZE(dirtyRects)       ||
    stagingFormatChanged                          ||
    m_nbDirtyRects == 0;

  if (!fullDamage && dirtyRectCount > 0)
  {
    IDARG_IN_GETDIRTYRECTS dirtyIn = {};
    dirtyIn.DirtyRectInCount = dirtyRectCount;
    dirtyIn.pDirtyRects = dirtyRects;

    IDARG_OUT_GETDIRTYRECTS dirtyOut = {};
    hr = IddCxSwapChainGetDirtyRects(m_hSwapChain, &dirtyIn, &dirtyOut);
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "IddCxSwapChainGetDirtyRects Failed");
      fullDamage = true;
    }
    else
      nbDirtyRects = dirtyOut.DirtyRectOutCount;
  }

  /* a window drag is delivered as MOVE REGIONS (the moved content) plus
   * dirty rects covering only the exposed edges — dropping the moves loses
   * the dragged window's damage and ghosts the drag (owner-reproducible,
   * 2026-07-06). The staging copy below is always full-frame, so the
   * DESTINATION rect is all the client needs; the vacated source area
   * arrives as regular dirty rects. */
  if (!fullDamage && moveRegionCount > 0)
  {
    IDDCX_MOVEREGION moves[LG_MAX_DIRTY_RECTS] = {};
    IDARG_IN_GETMOVEREGIONS moveIn = {};
    moveIn.MoveRegionInCount = moveRegionCount;
    moveIn.pMoveRegions = moves;

    IDARG_OUT_GETMOVEREGIONS moveOut = {};
    hr = IddCxSwapChainGetMoveRegions(m_hSwapChain, &moveIn, &moveOut);
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "IddCxSwapChainGetMoveRegions Failed");
      fullDamage = true;
    }
    else
    {
      for (unsigned i = 0; i < moveOut.MoveRegionOutCount; ++i)
      {
        if (nbDirtyRects >= ARRAYSIZE(dirtyRects))
        {
          fullDamage = true;
          break;
        }
        dirtyRects[nbDirtyRects++] = moves[i].DestRect;
      }
    }
  }

  if (fullDamage)
  {
    dirtyRects[0].left = 0;
    dirtyRects[0].top = 0;
    dirtyRects[0].right = static_cast<LONG>(desc.Width);
    dirtyRects[0].bottom = static_cast<LONG>(desc.Height);
    nbDirtyRects = 1;
  }
  ClipDirtyRects(dirtyRects, &nbDirtyRects, frameFormat.desc);

  /* fold in the damage owed from previously dropped frames — from here on,
   * dirtyRects is the debt-inclusive delivery set: a failure below banks it
   * back in full via AccumulatePendingDamage */
  if (m_pendingFullDamage ||
      m_nbPendingDamage + nbDirtyRects > ARRAYSIZE(dirtyRects))
  {
    dirtyRects[0].left   = 0;
    dirtyRects[0].top    = 0;
    dirtyRects[0].right  = static_cast<LONG>(desc.Width);
    dirtyRects[0].bottom = static_cast<LONG>(desc.Height);
    nbDirtyRects = 1;
  }
  else if (m_nbPendingDamage)
  {
    memcpy(dirtyRects + nbDirtyRects, m_pendingDamage,
      m_nbPendingDamage * sizeof(*dirtyRects));
    nbDirtyRects += m_nbPendingDamage;
  }
  m_pendingFullDamage = false;
  m_nbPendingDamage   = 0;
  const LONGLONG tRects = nowUs();

  if (stagingFormatChanged)
  {
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;

    m_d11Staging.Reset();
    DEBUG_TRACE("D3D11 fallback: CreateTexture2D staging");
    hr = m_dx11Device->GetBaseDevice()->CreateTexture2D(&stagingDesc, nullptr, &m_d11Staging);
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "Failed to create D3D11 staging texture");
      AccumulatePendingDamage(dirtyRects, nbDirtyRects);
      return false;
    }
    m_d11StagingDesc = stagingDesc;
    ++m_d11Stats.stagingRecreates;
  }
  ID3D11Texture2D * staging = m_d11Staging.Get();
  const LONGLONG tStaging = nowUs();

  const bool copyFullFrame =
    IsFullDamage(dirtyRects, nbDirtyRects, frameFormat.desc) ||
    m_nbDirtyRects + nbDirtyRects > ARRAYSIZE(dirtyRects);

  auto context = m_dx11Device->GetBaseContext();
  if (copyFullFrame)
  {
    DEBUG_TRACE("D3D11 fallback: CopyResource staging<-swapchain");
    context->CopyResource(staging, texture.Get());
  }
  else
  {
    DEBUG_TRACE("D3D11 fallback: CopySubresourceRegion staging<-swapchain");
    auto copyRectToStaging = [&](RECT rect)
    {
      if (!ClipDirtyRect(rect, frameFormat.desc))
        return;

      D3D11_BOX box = {};
      box.left   = (UINT)rect.left;
      box.top    = (UINT)rect.top;
      box.front  = 0;
      box.right  = (UINT)rect.right;
      box.bottom = (UINT)rect.bottom;
      box.back   = 1;

      context->CopySubresourceRegion(staging, 0,
        (UINT)rect.left, (UINT)rect.top, 0, texture.Get(), 0, &box);
    };

    for (const RECT * rect = m_dirtyRects; rect < m_dirtyRects + m_nbDirtyRects; ++rect)
      copyRectToStaging(*rect);

    for (const RECT * rect = dirtyRects; rect < dirtyRects + nbDirtyRects; ++rect)
      copyRectToStaging(*rect);
  }

  acquiredBuffer.Reset();
  texture.Reset();
  const LONGLONG tCopy = nowUs();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  DEBUG_TRACE("D3D11 fallback: Map staging");
  hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to map D3D11 staging texture");
    AccumulatePendingDamage(dirtyRects, nbDirtyRects);
    return false;
  }
  const LONGLONG tMap = nowUs();

  auto buffer = m_devContext->PrepareFrameBuffer(mapped.RowPitch, frameFormat, frameFormat, dirtyRects, nbDirtyRects);
  if (!buffer.mem)
  {
    DEBUG_ERROR("Failed to prepare D3D11 fallback frame buffer");
    context->Unmap(staging, 0);
    AccumulatePendingDamage(dirtyRects, nbDirtyRects);
    return false;
  }

  const size_t bpp = frameType == FRAME_TYPE_RGBA16F ? 8 : 4;
  const size_t srcRowBytes = (size_t)desc.Width * bpp;
  const size_t rowBytes = srcRowBytes < mapped.RowPitch ? srcRowBytes : mapped.RowPitch;
  size_t rows = desc.Height < buffer.height ? desc.Height : buffer.height;

  /* one-shot bandwidth probe: is the ~35ms frame memcpy bound by READS of the
   * venus-mapped staging (cache attribute = the host's per-blob map_info) or
   * WRITES to the write-combined IVSHMEM mapping? 4 MiB each way. */
  if (!m_bwProbed && rows * rowBytes >= (4u << 20))
  {
    m_bwProbed = true;
    const size_t probeBytes = 4u << 20;
    const uint8_t * src = static_cast<const uint8_t *>(mapped.pData);
    volatile uint64_t sink = 0;
    const LONGLONG r0 = nowUs();
    for (size_t i = 0; i < probeBytes; i += 64)
      sink += src[i];
    const LONGLONG r1 = nowUs();
    memset(buffer.mem, 0, probeBytes);
    const LONGLONG w1 = nowUs();
    memcpy(buffer.mem, src, probeBytes);
    const LONGLONG c1 = nowUs();
    CopyFromWC(buffer.mem, src, probeBytes);
    const LONGLONG s1 = nowUs();
    DEBUG_INFO("BW probe (4MiB): src strided-read %.0f MB/s | dst memset %.0f MB/s | "
               "memcpy %.0f MB/s | streamcpy %.0f MB/s (sink=%llu)",
      4.0 * 1e6 / (double)(r1 - r0),
      4.0 * 1e6 / (double)(w1 - r1),
      4.0 * 1e6 / (double)(c1 - w1),
      4.0 * 1e6 / (double)(s1 - c1),
      (unsigned long long)sink);
  }

  const uint8_t * src = static_cast<const uint8_t *>(mapped.pData);
  uint8_t * dst = buffer.mem;
  size_t copiedRows = 0;
  size_t copiedBytes = 0;
  unsigned copiedRects = 0;
  uint64_t sampleHash = 1469598103934665603ull;
  uint32_t sampleNonZero = 0;
  uint32_t sampleBytes = 0;
  uint32_t firstPixel = 0;
  uint32_t centerPixel = 0;
  if (rows > 0 && rowBytes >= 4)
  {
    memcpy(&firstPixel, src, sizeof(firstPixel));
    const size_t centerRow = rows / 2;
    const size_t centerCol = (rowBytes / bpp) / 2;
    memcpy(&centerPixel,
      src + centerRow * mapped.RowPitch + centerCol * bpp,
      sizeof(centerPixel));
  }
  auto copyMappedRect = [&](RECT rect)
  {
    if (!ClipDirtyRect(rect, frameFormat.desc))
      return;

    if (rect.top >= (LONG)rows)
      return;
    if (rect.bottom > (LONG)rows)
      rect.bottom = (LONG)rows;

    const size_t leftBytes = (size_t)rect.left * bpp;
    if (leftBytes >= rowBytes || leftBytes >= buffer.pitch)
      return;

    const size_t rectBytes = (size_t)(rect.right - rect.left) * bpp;
    const size_t srcAvail = rowBytes - leftBytes;
    const size_t dstAvail = buffer.pitch - leftBytes;
    const size_t copyBytes = min(rectBytes, min(srcAvail, dstAvail));
    if (copyBytes == 0)
      return;

    for (LONG row = rect.top; row < rect.bottom; ++row)
    {
      CopyFromWC(dst + (size_t)row * buffer.pitch + leftBytes,
        src + (size_t)row * mapped.RowPitch + leftBytes, copyBytes);
      ++copiedRows;
      copiedBytes += copyBytes;
    }
    ++copiedRects;
  };

  if (copyFullFrame)
  {
    for (size_t row = 0; row < rows; ++row)
    {
      const size_t copyBytes = rowBytes < buffer.pitch ? rowBytes : buffer.pitch;
      CopyFromWC(dst + row * buffer.pitch, src + row * mapped.RowPitch, copyBytes);
      if (row == 0 || row == rows / 2 || row + 1 == rows)
      {
        const uint8_t * sample = src + row * mapped.RowPitch;
        for (size_t i = 0; i < copyBytes; i += 64)
        {
          const uint8_t v = sample[i];
          sampleNonZero += v != 0;
          sampleHash ^= v;
          sampleHash *= 1099511628211ull;
          ++sampleBytes;
        }
      }
      if (buffer.pitch > copyBytes)
        memset(dst + row * buffer.pitch + copyBytes, 0, buffer.pitch - copyBytes);
      ++copiedRows;
      copiedBytes += copyBytes;
    }
    copiedRects = 1;
  }
  else
  {
    for (const RECT * rect = m_dirtyRects; rect < m_dirtyRects + m_nbDirtyRects; ++rect)
      copyMappedRect(*rect);

    for (const RECT * rect = dirtyRects; rect < dirtyRects + nbDirtyRects; ++rect)
      copyMappedRect(*rect);
  }
  context->Unmap(staging, 0);
  const LONGLONG tMemcpy = nowUs();

  m_devContext->FinalizeFrameBuffer(buffer.frameIndex, rows * buffer.pitch);
  m_devContext->PresentHeliosFrame(buffer.frameIndex);
  memcpy(m_dirtyRects, dirtyRects, nbDirtyRects * sizeof(*m_dirtyRects));
  m_nbDirtyRects = nbDirtyRects;
  const LONGLONG tLgmp = nowUs();

  /* per-stage aggregate: the swapchain thread is fully serialized, so these
   * stages ARE the delivered frame rate — one INFO line per 300 frames */
  {
    auto & s = m_d11Stats;
    if (s.frames == 0)
      s.windowStart = tEnter;
    const LONGLONG gap = s.lastFrameEnd ? tEnter - s.lastFrameEnd : 0;
    s.lastFrameEnd = tLgmp;
    s.rects       += tRects   - tEnter;
    s.staging     += tStaging - tRects;
    s.copy        += tCopy    - tStaging;
    s.map         += tMap     - tCopy;
    s.memcpyKvmfr += tMemcpy  - tMap;
    s.lgmp        += tLgmp    - tMemcpy;
    s.gap         += gap;
    if (tMap - tCopy > s.mapMax) s.mapMax = tMap - tCopy;
    if (gap > s.gapMax)          s.gapMax = gap;
    if (++s.frames >= 300)
    {
      const double windowSec = (double)(tLgmp - s.windowStart) / 1e6;
      DEBUG_INFO("D3D11 path stats: %.1f fps over %.1fs | us avg: rects=%lld staging=%lld "
                 "copy=%lld map=%lld (max %lld) memcpy=%lld lgmp=%lld gap=%lld (max %lld) "
                 "| staging recreates=%u",
        s.frames / windowSec, windowSec,
        s.rects / s.frames, s.staging / s.frames, s.copy / s.frames,
        s.map / s.frames, s.mapMax, s.memcpyKvmfr / s.frames, s.lgmp / s.frames,
        s.gap / s.frames, s.gapMax, s.stagingRecreates);
      memset(&s, 0, sizeof(s));
      s.lastFrameEnd = tLgmp;
    }
  }

  const LONG frames = InterlockedIncrement(&m_systemFrameCount);
  if (frames == 1 || (frames % 30) == 0)
    DEBUG_INFO("D3D11 fallback frame %ld: %ux%u srcPitch=%u dstPitch=%u format=0x%x dirty=%u copy=%s rects=%u copiedRows=%llu copiedBytes=%llu rows=%llu rowBytes=%llu first=0x%08x center=0x%08x sampleNonZero=%u/%u sampleHash=0x%llx",
      frames, desc.Width, desc.Height, mapped.RowPitch, buffer.pitch,
      desc.Format, nbDirtyRects, copyFullFrame ? "full" : "dirty", copiedRects,
      (unsigned long long)copiedRows, (unsigned long long)copiedBytes,
      (unsigned long long)rows, (unsigned long long)rowBytes,
      firstPixel, centerPixel, sampleNonZero, sampleBytes,
      (unsigned long long)sampleHash);

  return true;
}

bool CSwapChainProcessor::SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer, unsigned dirtyRectCount)
{
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = acquiredBuffer.As(&texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to obtain the ID3D11Texture2D from the acquiredBuffer");
    return false;
  }

  CInteropResource * srcRes = m_resPool.Get(texture);
  if (!srcRes)
  {
    DEBUG_ERROR("Failed to get a CInteropResource from the pool");
    return false;
  }

  /**
   * Even though we have not performed any copy/draw operations we still need to
   * use a fence. Because we share this texture with DirectX12 it is able to
   * read from it before the desktop duplication API has finished updating it.
   */
  srcRes->Signal();

  RECT dirtyRects[LG_MAX_DIRTY_RECTS] = {0};
  if (dirtyRectCount > ARRAYSIZE(dirtyRects))
  {
    srcRes->SetFullDamage();
  }
  else
  {
    IDARG_IN_GETDIRTYRECTS dirtyIn = {};
    dirtyIn.DirtyRectInCount = dirtyRectCount;
    dirtyIn.pDirtyRects      = dirtyRects;

    IDARG_OUT_GETDIRTYRECTS dirtyOut = {};
    hr = IddCxSwapChainGetDirtyRects(m_hSwapChain, &dirtyIn, &dirtyOut);
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "IddCxSwapChainGetDirtyRects Failed");
      srcRes->SetFullDamage();
    }
    else
      srcRes->SetDirtyRects(dirtyRects, dirtyOut.DirtyRectOutCount);
  }

  D3D12_RESOURCE_DESC srcDesc = srcRes->GetRes()->GetDesc();
  D12FrameFormat srcFormat = {};
  srcFormat.desc   = srcDesc;
  srcFormat.width  = (unsigned)srcDesc.Width;
  srcFormat.height = srcDesc.Height;
  srcFormat.format = GetFrameType(srcDesc.Format);
  srcFormat.hdr    = srcDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  srcFormat.hdrPQ  = false;

  bool postProcessFormatChanged = false;
  if (!m_postProcessor.Configure(srcFormat, &postProcessFormatChanged))
    return false;

  if (postProcessFormatChanged)
    m_nbDirtyRects = 0;

  const D12FrameFormat& dstFormat = m_postProcessor.GetOutputFormat();

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
  m_dx12Device->GetDevice()->GetCopyableFootprints(
    &dstFormat.desc,
    0,
    1,
    0,
    &layout,
    NULL,
    NULL,
    NULL);

  RECT currentDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  RECT frameDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbDirtyRects = srcRes->GetDirtyRectCount();
  if (nbDirtyRects > ARRAYSIZE(currentDirtyRects))
    nbDirtyRects = 0;
  else
  {
    memcpy(currentDirtyRects, srcRes->GetDirtyRects(), nbDirtyRects * sizeof(*currentDirtyRects));
    memcpy(frameDirtyRects, currentDirtyRects, nbDirtyRects * sizeof(*frameDirtyRects));
  }
  unsigned frameDirtyRectCount = nbDirtyRects;
  m_postProcessor.AdjustFrameDamage(frameDirtyRects, &frameDirtyRectCount);

  auto copyQueue = m_dx12Device->TryGetCopyQueue();
  if (!copyQueue)
  {
    const LONG drops = InterlockedIncrement(&m_copyBusyDrops);
    if (drops == 1 || (drops % 300) == 0)
      DEBUG_WARN("D3D12 copy queues busy, dropping frame (drops=%ld)", drops);
    return false;
  }

  ComPtr<ID3D12Resource> copySrcResource = srcRes->GetRes();
  CD3D12CommandQueue * computeQueue = nullptr;
  if (m_postProcessor.HasActiveEffects())
  {
    computeQueue = m_dx12Device->GetComputeQueue();
    if (!computeQueue)
    {
      DEBUG_ERROR("Failed to get a ComputeQueue");
      return false;
    }

    srcRes->Sync(*computeQueue);
    copySrcResource = m_postProcessor.Run(
      computeQueue->GetGfxList(), copySrcResource,
      currentDirtyRects, &nbDirtyRects);

    computeQueue->Execute();
    copyQueue->WaitFor(*computeQueue);
  }
  else
    srcRes->Sync(*copyQueue);

  ClipDirtyRects(currentDirtyRects, &nbDirtyRects, dstFormat.desc);

  auto buffer = m_devContext->PrepareFrameBuffer(
    (unsigned)layout.Footprint.RowPitch,
    srcFormat,
    dstFormat,
    frameDirtyRects,
    frameDirtyRectCount);

  if (!buffer.mem)
    return false;

  CFrameBufferResource * fbRes = m_fbPool.Get(buffer,
    (size_t)layout.Footprint.RowPitch * dstFormat.desc.Height);

  if (!fbRes)
  {
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
    return false;
  }

  copyQueue->SetCompletionCallback(&CompletionFunction, this, fbRes);

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource        = copySrcResource.Get();
  srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource       = fbRes->Get().Get();
  dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint = layout;

  if (IsFullDamage(currentDirtyRects, nbDirtyRects, dstFormat.desc) ||
      nbDirtyRects > KVMFR_MAX_DAMAGE_RECTS || m_nbDirtyRects == 0)
  {
    copyQueue->GetGfxList()->CopyTextureRegion(
      &dstLoc, 0, 0, 0, &srcLoc, NULL);
  }
  else if (m_nbDirtyRects + nbDirtyRects > LG_MAX_DIRTY_RECTS)
  {
    copyQueue->GetGfxList()->CopyTextureRegion(
      &dstLoc, 0, 0, 0, &srcLoc, NULL);
  }
  else
  {
    for (const RECT * rect = m_dirtyRects; rect < m_dirtyRects + m_nbDirtyRects; ++rect)
    {
      RECT clipped = *rect;
      if (ClipDirtyRect(clipped, dstFormat.desc))
        CopyDirtyRect(copyQueue->GetGfxList(), &dstLoc, &srcLoc, clipped);
    }

    for (const RECT * rect = currentDirtyRects; rect < currentDirtyRects + nbDirtyRects; ++rect)
      CopyDirtyRect(copyQueue->GetGfxList(), &dstLoc, &srcLoc, *rect);
  }

  memcpy(m_dirtyRects, currentDirtyRects, nbDirtyRects * sizeof(*m_dirtyRects));
  m_nbDirtyRects = nbDirtyRects;

  copyQueue->Execute();

  return true;
}

DWORD CALLBACK CSwapChainProcessor::_CursorThread(LPVOID arg)
{
  auto* self = reinterpret_cast<std::shared_ptr<CSwapChainProcessor>*>(arg);
  (*self)->CursorThread();
  delete self;
  return 0;
}

bool CSwapChainProcessor::QueryHWCursor()
{
  IDARG_IN_QUERY_HWCURSOR in = {};
  in.LastShapeId            = m_lastShapeId;
  in.pShapeBuffer           = m_shapeBuffer;
  in.ShapeBufferSizeInBytes = 512 * 512 * 4;

  IDARG_OUT_QUERY_HWCURSOR out = {};
  NTSTATUS status;
#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  if (m_devContext->CanProcessFP16())
  {
    IDARG_OUT_QUERY_HWCURSOR3 out3 = {};
    status = IddCxMonitorQueryHardwareCursor3(m_monitor, &in, &out3);
    out.IsCursorVisible      = out3.IsCursorVisible;
    out.X                    = out3.X;
    out.Y                    = out3.Y;
    out.IsCursorShapeUpdated = out3.IsCursorShapeUpdated;
    out.CursorShapeInfo      = out3.CursorShapeInfo;
  }
  else
#endif
  {
    status = IddCxMonitorQueryHardwareCursor(m_monitor, &in, &out);
  }

  if (FAILED(status))
  {
    // this occurs if the display went away (ie, screen blanking or disabled)
    if (status == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
    {
      m_devContext->OnSwapChainLost();
      /* self-initiated terminate: the main loop will see the terminate event
       * and exit "normally", but this is a self-exit and must escalate to
       * the monitor lifecycle (see SwapChainThread) */
      InterlockedExchange(&m_selfTerminated, 1);
      SetEvent(m_terminateEvent.Get());
      return false;
    }

    DEBUG_ERROR("IddCxMonitorQueryHardwareCursor failed (0x%08x)", status);
    return false;
  }

  if (out.IsCursorShapeUpdated)
    m_lastShapeId = out.CursorShapeInfo.ShapeId;

  m_devContext->SendCursor(out, m_shapeBuffer);
  return true;
}

void CSwapChainProcessor::CursorThread()
{
  HRESULT hr = 0;
  bool running = true;

  while (running)
  {
    HANDLE waitHandles[] =
    {
      m_cursorDataEvent.Get(),
      m_terminateEvent.Get()
    };

    DWORD waitResult = WaitForMultipleObjects(
      ARRAYSIZE(waitHandles), waitHandles, FALSE, 100);

    switch (waitResult)
    {
    case WAIT_TIMEOUT:
      continue;

      // cursorDataEvent
    case WAIT_OBJECT_0:
      if (!QueryHWCursor())
        return;
      continue;

      // terminateEvent
    case WAIT_OBJECT_0 + 1:
      running = false;
      continue;

    default:
      hr = HRESULT_FROM_WIN32(waitResult);
      DEBUG_ERROR_HR(hr, "WaitForMultipleObjects");
      return;
    }
  }
}
