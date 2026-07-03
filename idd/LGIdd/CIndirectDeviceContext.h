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

#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include "CIVSHMEM.h"
#include "CSettings.h"
#include "CEdid.h"
#include "CHeliosSink.h"
#include "CPostProcessor.h"

extern "C" {
  #include "lgmp/host.h"
}

#include "common/KVMFR.h"
#define MAX_POINTER_SIZE (sizeof(KVMFRCursor) + (512 * 512 * 4))
#define POINTER_SHAPE_BUFFERS 3

//FIXME: this should not really be done here, this is a hack
#pragma warning(push)
#pragma warning(disable: 4200)
struct FrameBuffer
{
  volatile uint32_t wp;
  uint8_t data[0];
};
#pragma warning(pop)

class CIndirectDeviceContext
{
public:
  /* C5 monitor-lifecycle state machine.
   *
   * IddCx drivers own their monitor lifecycle: the OS is free to abandon and
   * re-create swapchains and re-pair render adapters at any time, and a
   * correct driver converges from every such event with no external help.
   * The previous implementation stalled permanently in two ways:
   *  - re-arrival after IddCxMonitorDeparture only happened in
   *    OnUnassignedSwapChain, which the OS only calls if a swapchain WAS
   *    assigned — a departure with no assigned swapchain latched the old
   *    m_replugMonitor flag forever and the monitor never returned;
   *  - nothing ever retried when the OS simply never offered a swapchain
   *    (boot-time budget exhaustion / stale pairing after a dwm restart).
   * Every state below has a watchdog-driven exit, so the machine cannot sit
   * in any state forever. */
  enum class MonitorState : LONG
  {
    NoMonitor      = 0, // no live monitor object; watchdog re-runs FinishInit
    Arrived        = 1, // MonitorArrival OK, waiting for AssignSwapChain
    SwapChainBound = 2, // swapchain assigned, processor running
    ReplugPending  = 3, // departure issued; FinishInit re-runs on unassign OR timeout
  };

private:
  WDFDEVICE     m_wdfDevice;
  IDDCX_ADAPTER m_adapter       = nullptr;
  IDDCX_MONITOR m_monitor       = nullptr;

  /* state machine bookkeeping — m_stateLock guards transitions and the
   * FinishInit/ReplugMonitor bodies (CRITICAL_SECTION is reentrant per
   * thread, so watchdog-driven transitions may call them while holding it) */
  CRITICAL_SECTION m_stateLock = {};
  volatile LONG m_monitorState    = (LONG)MonitorState::NoMonitor;
  ULONGLONG     m_stateEnteredTick = 0;
  volatile LONG m_unassignSeen    = 0; // OS confirmed the departure
  volatile LONG m_firstFrameSeen  = 0; // >=1 acquire since the last bind
  volatile LONG m_watchdogActive  = 0;
  /* EvtIddCxMonitorAssignSwapChain is executing / the processor thread is
   * inside IddCxSwapChainSetDevice. While either is set, ReplugMonitor
   * DEFERS: departing the monitor makes IddCx tear down its objects under
   * the running call — observed as FAST_FAIL_CORRUPT_LIST_ENTRY (0xc0000409
   * data=7) at iddcx.dll+0xa631 killing WUDFHost (boot #4, 2026-07-03). */
  volatile LONG m_assignInFlight  = 0;
  volatile LONG m_bindingInFlight = 0;
  /* nonzero while the processor thread is between a successful
   * ReleaseAndAcquireBuffer and IddCxSwapChainFinishedProcessingFrame.
   * Departing the monitor then destroys the child swapchain with the frame
   * outstanding and IddCx kills the host process
   * (ReportBugcheckForSwapChainTimeoutDriverDidNotReleaseFrame — observed
   * 2026-07-03 10:25:52Z, dump WUDFHost.exe-(PID-8464)). ReplugMonitor
   * defers while this is set; the hold window is tiny, so retries succeed
   * within a watchdog/LGMP tick. */
  volatile LONG m_frameHeld       = 0;
  bool          m_renderAdapterSelected = false; // latch: issue once per adapter
  WDFTIMER      m_watchdogTimer = nullptr;

  void EnterState(MonitorState s);

  CIVSHMEM m_ivshmem;
  CHeliosSink m_heliosSink;

  PLGMPHost      m_lgmp       = nullptr;
  WDFTIMER       m_lgmpTimer  = nullptr;
  PLGMPHostQueue m_frameQueue = nullptr;
  volatile LONG  m_lgmpProcessActive = 0;

  PLGMPHostQueue m_pointerQueue = nullptr;
  PLGMPMemory    m_pointerMemory     [LGMP_Q_POINTER_LEN   ] = {};
  PLGMPMemory    m_pointerShapeMemory[POINTER_SHAPE_BUFFERS] = {};
  PLGMPMemory    m_pointerShape = nullptr;
  int m_pointerMemoryIndex = 0;
  int m_pointerShapeIndex  = 0;
  bool m_cursorVisible = false;
  int m_cursorX = 0, m_cursorY = 0;

  size_t         m_alignSize    = 0;
  size_t         m_maxFrameSize = 0;
  int            m_frameIndex   = 0;
  uint32_t       m_formatVer    = 0;
  uint32_t       m_frameSerial  = 0;
  PLGMPMemory    m_frameMemory[LGMP_Q_FRAME_LEN] = {};
  KVMFRFrame   * m_frame      [LGMP_Q_FRAME_LEN] = {};
  FrameBuffer  * m_frameBuffer[LGMP_Q_FRAME_LEN] = {};
  CRITICAL_SECTION m_frameLock = {};
  volatile LONG  m_frameDropCount = 0;

  unsigned    m_width    = 0;
  unsigned    m_height   = 0;
  unsigned    m_pitch    = 0;
  DXGI_FORMAT m_format   = DXGI_FORMAT_UNKNOWN;
  FrameType   m_frameType = FRAME_TYPE_INVALID;
  bool        m_hasFrame = false;

  UINT m_iddCxVersion = 0;
  bool m_canProcessFP16 = false;

  void QueryIddCxCapabilities();
  bool CanUseIddCx110DDIs() const { return m_canProcessFP16; }

  void DeInitLGMP();
  void LGMPTimer();
  void ProcessLGMP(bool allowReplug);
  void ResendCursor() const;
  bool UpdateMonitorModes();

  CSettings::DisplayModes m_displayModes;
  CEdid                   m_edid;

  CSettings::DisplayMode m_setMode = {};
  bool m_doSetMode = false;
  volatile LONG m_replugMonitorQueued = 0;
  volatile LONG m_recoverModeUpdateSwapChain = 0;

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice)
  {
    InitializeCriticalSection(&m_frameLock);
    InitializeCriticalSection(&m_stateLock);
  };

  virtual ~CIndirectDeviceContext()
  {
    if (m_watchdogTimer)
    {
      WdfTimerStop(m_watchdogTimer, TRUE);
      m_watchdogTimer = nullptr;
    }
    m_heliosSink.Deinit();
    DeInitLGMP();
    DeleteCriticalSection(&m_stateLock);
    DeleteCriticalSection(&m_frameLock);
  }

  bool SetupLGMP(size_t alignSize);

  void PopulateDefaultModes();
  void InitAdapter();
  void SelectRenderAdapter();
  void FinishInit(UINT connectorIndex);
  void ReplugMonitor();
  void MonitorWatchdog();

  void OnAssignSwapChain();
  void OnUnassignedSwapChain();
  void OnSwapChainLost();
  void OnSwapChainDeviceLost();

  /* brackets the EvtIddCxMonitorAssignSwapChain callback (Device.cpp) */
  void AssignCallbackEnter();
  void AssignCallbackExit();

  /* brackets the processor thread's IddCxSwapChainSetDevice attempts; Done
   * restarts the first-frame (stale-binding) clock so it measures from the
   * moment the device was actually bound, not from the assignment */
  void NotifyBindingStart();
  void NotifyBindingDone();

  /* called by CSwapChainProcessor on every successful buffer acquire —
   * disarms the first-frame (stale-binding) watchdog for this binding */
  void NotifyFrameAcquired();

  /* brackets the processor thread's acquire → FinishedProcessingFrame
   * window; ReplugMonitor must not depart the monitor while a frame is
   * outstanding (see m_frameHeld) */
  void NotifyFrameHeld();
  void NotifyFrameReleased();

  /* called by CSwapChainProcessor when its processing thread exits for any
   * reason other than a normal terminate request — the swapchain is dead but
   * the OS told us nothing, so schedule a monitor replug */
  void NotifySwapChainSelfExit();

  NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs);
  NTSTATUS MonitorGetDefaultModes(
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs);
  NTSTATUS MonitorQueryTargetModes(
    const IDARG_IN_QUERYTARGETMODES* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs);

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs);
  NTSTATUS MonitorQueryTargetModes2(
    const IDARG_IN_QUERYTARGETMODES2* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs);
#endif

  void SetResolution(int width, int height);

  size_t GetAlignSize   () const { return m_alignSize     ; }
  size_t GetMaxFrameSize() const { return m_maxFrameSize  ; }
  bool   CanProcessFP16 () const { return m_canProcessFP16; }

  struct PreparedFrameBuffer
  {
    unsigned frameIndex;
    uint8_t* mem;
    unsigned pitch;
    unsigned height;
  };

  PreparedFrameBuffer PrepareFrameBuffer(unsigned pitch, const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat, const RECT * dirtyRects, unsigned nbDirtyRects);
  void WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos) const;
  void FinalizeFrameBuffer(unsigned frameIndex, size_t len) const;
  void FinalizeFrameBuffer(unsigned frameIndex) const;
  void PresentHeliosFrame(unsigned frameIndex);

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR & info, const BYTE * data);

  CIVSHMEM &GetIVSHMEM() { return m_ivshmem; }
};

struct CIndirectDeviceContextWrapper
{
  CIndirectDeviceContext* context;

  void Cleanup()
  {
    delete context;
    context = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE(CIndirectDeviceContextWrapper);
