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

#include "CIndirectDeviceContext.h"
#include "CIndirectMonitorContext.h"

#include "CSettings.h"
#include "CPlatformInfo.h"
#include "CPipeServer.h"
#include "CDebug.h"
#include "VersionInfo.h"

#include <sstream>

static const struct LGMPQueueConfig FRAME_QUEUE_CONFIG =
{
  LGMP_Q_FRAME,       //queueID
  LGMP_Q_FRAME_LEN,   //numMessages
  1000                //subTimeout
};

static const struct LGMPQueueConfig POINTER_QUEUE_CONFIG =
{
  LGMP_Q_POINTER,     //queueID
  LGMP_Q_POINTER_LEN, //numMesages
  1000                //subTimeout
};

static const UINT IDDCX_VERSION_1_10 = 0x1A00;

/* C5 monitor-lifecycle watchdog periods (ms). Every state has an exit:
 * NoMonitor      → re-run FinishInit after NOMONITOR_RETRY_MS
 * Arrived        → replug if no AssignSwapChain within the offer window.
 *                  The window BACKS OFF exponentially with consecutive
 *                  fruitless replugs (10/20/40/80 s, reset by a real acquired
 *                  frame): dwm needs 10 s+ to rebuild composition after churn,
 *                  and replugging mid-rebuild restarts that clock — a fixed
 *                  10 s window livelocks the pairing (observed 2026-07-04).
 * SwapChainBound → two sub-cases:
 *                  (1) no frame yet: replug if zero acquires within
 *                  FIRSTFRAME_TIMEOUT_MS measured from DEVICE-READY (the
 *                  processor's D3D bring-up on a churning venus stack takes
 *                  10-15 s; measuring from bind killed legitimate pairings),
 *                  with DEVICEINIT_TIMEOUT_MS as the absolute cap in case the
 *                  device init itself never completes.
 *                  (2) after the first frame: a previously-flowing binding
 *                  whose acquires stop for MIDSTREAM_STALL_MS gets ONE replug
 *                  (per binding) — covers dwm feeding an orphaned swapchain
 *                  instance after a WUDFHost restart; the once-per-binding
 *                  latch bounds the cost of an idle-desktop false positive.
 * ReplugPending  → re-run FinishInit when the OS confirms the departure
 *                  (UnassignSwapChain) OR unconditionally after
 *                  REPLUG_SETTLE_MS — the OS only calls UnassignSwapChain if
 *                  a swapchain WAS assigned, so the forced path is what makes
 *                  the replug primitive always complete. */
static const ULONGLONG WATCHDOG_PERIOD_MS   = 500;
static const ULONGLONG NOMONITOR_RETRY_MS   = 5000;
static const ULONGLONG OFFER_TIMEOUT_MS     = 10000;
static const ULONGLONG FIRSTFRAME_TIMEOUT_MS = 10000;
static const ULONGLONG REPLUG_SETTLE_MS     = 1000;
static const ULONGLONG DEVICEINIT_TIMEOUT_MS = 45000;
static const ULONGLONG MIDSTREAM_STALL_MS   = 45000;
static const LONG      OFFER_BACKOFF_MAX_SHIFT = 3;

void CIndirectDeviceContext::EnterState(MonitorState s)
{
  InterlockedExchange(&m_monitorState, (LONG)s);
  m_stateEnteredTick = GetTickCount64();
}

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
static inline IDDCX_WIRE_BITS_PER_COMPONENT GetWireBitsPerComponent(bool hdr)
{
  IDDCX_WIRE_BITS_PER_COMPONENT bits = {};
  /* If IddCx 1.10 HDR/WCG support is available, prefer a 10 bpc scanout. The
   * frame pipeline already handles DXGI_FORMAT_R10G10B10A2_UNORM as
   * FRAME_TYPE_RGBA10, while advertising both 8 and 10 lets Windows pick 8 bpc.
   */
  bits.Rgb = hdr ? IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8;
  bits.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
  return bits;
}
#endif

void CIndirectDeviceContext::QueryIddCxCapabilities()
{
  IDARG_OUT_GETVERSION ver = {};
  NTSTATUS status = IddCxGetVersion(&ver);
  if (!NT_SUCCESS(status))
  {
    m_iddCxVersion = 0;
    m_canProcessFP16 = false;
    DEBUG_ERROR_HR(status, "IddCxGetVersion Failed");
    return;
  }

  m_iddCxVersion = ver.IddCxVersion;

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  const bool hasIddCx110DDIs =
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2) &&
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3) &&
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorQueryTargetModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetDefaultHdrMetaData) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp);
#else
  const bool hasIddCx110DDIs = false;
#endif

  // Helios currently uses the D3D11 readback path. Keep the IDD on the legacy
  // acquire APIs until the Helios path can prove stable FP16/HDR handling.
  m_canProcessFP16 = false;

  DEBUG_INFO("IddCx version: 0x%04x", m_iddCxVersion);
  DEBUG_INFO("IddCx 1.10 HDR/WCG DDIs: %s (runtime support: %s)",
    m_canProcessFP16 ? "enabled" : "disabled",
    (m_iddCxVersion >= IDDCX_VERSION_1_10 && hasIddCx110DDIs) ? "available" : "unavailable");
}

void CIndirectDeviceContext::PopulateDefaultModes()
{
  g_settings.LoadModes();

  m_displayModes.clear();
  m_displayModes.reserve(g_settings.GetDisplayModes().size());
  for (auto& dm : g_settings.GetDisplayModes())
    m_displayModes.push_back(dm);

  m_edid.Build(m_displayModes);
}

void CIndirectDeviceContext::InitAdapter()
{
  if (!m_ivshmem.Init() || !m_ivshmem.Open())
    return;

  QueryIddCxCapabilities();
  PopulateDefaultModes();

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);

  /**
   * For some reason if we do not set this flag sometimes windows will
   * refuse to enumerate our virtual monitor. Intel also noted in their
   * sources that if this is not set dynamic resolution changes from this
   * driver will not work. This behaviour is not documented by Microsoft.
   */
  caps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;
#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  if (CanUseIddCx110DDIs())
    caps.Flags |= IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
#endif

  caps.MaxMonitorsSupported = 1;

  caps.EndPointDiagnostics.Size             = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport     = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;

  caps.EndPointDiagnostics.pEndPointFriendlyName     = L"Looking Glass IDD Driver";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"Looking Glass";
  caps.EndPointDiagnostics.pEndPointModelName        = L"Looking Glass";

  IDDCX_ENDPOINT_VERSION ver = {};
  ver.Size     = sizeof(ver);
  ver.MajorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &ver;
  caps.EndPointDiagnostics.pHardwareVersion = &ver;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectDeviceContextWrapper);

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice        = m_wdfDevice;
  init.pCaps            = &caps;
  init.ObjectAttributes = &attr;

  IDARG_OUT_ADAPTER_INIT initOut;
  NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
  if (!NT_SUCCESS(status) && CanUseIddCx110DDIs())
  {
    DEBUG_WARN(
      "IddCxAdapterInitAsync rejected FP16 adapter capabilities (0x%08x), retrying without HDR/WCG",
      status);
    m_canProcessFP16 = false;
    caps.Flags = (IDDCX_ADAPTER_FLAGS)(caps.Flags & ~IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16);
    ZeroMemory(&initOut, sizeof(initOut));
    status = IddCxAdapterInitAsync(&init, &initOut);
  }

  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxAdapterInitAsync Failed");
    return;
  }

  m_adapter = initOut.AdapterObject;

  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(m_adapter);
  wrapper->context = this;

  // Render-adapter selection happens in FinishInit (see SelectRenderAdapter):
  // calling IddCxAdapterSetRenderAdapter here, before the async adapter init
  // has completed, is silently rejected and the OS keeps its own pick.

  /* C5: the monitor-lifecycle watchdog. Deliberately NOT the LGMP timer: the
   * LGMP timer only exists once the first swapchain arrives (SetupLGMP), and
   * the most important watchdog case is exactly "the first swapchain never
   * arrived". This timer lives from adapter init so boot-time convergence is
   * covered too. */
  {
    WDF_TIMER_CONFIG config;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&config,
      [](WDFTIMER timer) -> void
      {
        WDFOBJECT parent = WdfTimerGetParentObject(timer);
        auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(parent);
        wrapper->context->MonitorWatchdog();
      },
      (ULONG)WATCHDOG_PERIOD_MS);
    config.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES attribs;
    WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.ParentObject   = m_wdfDevice;
    attribs.ExecutionLevel = WdfExecutionLevelDispatch;

    NTSTATUS ts = WdfTimerCreate(&config, &attribs, &m_watchdogTimer);
    if (!NT_SUCCESS(ts))
      DEBUG_ERROR_HR(ts, "Monitor watchdog timer creation failed; "
                         "lifecycle self-convergence is unavailable");
    else
      WdfTimerStart(m_watchdogTimer, WDF_REL_TIMEOUT_IN_MS((ULONG)WATCHDOG_PERIOD_MS));
  }

  DEBUG_INFO("Helios: IDD Vulkan output sink disabled; using normal Looking Glass capture path");
}

// Render-adapter selection for the IDD compositor.
//
// Default (HKLM\SOFTWARE\LookingGlass\IDD\HeliosRenderAdapter == 0): force the
// IDD onto the software render adapter (Microsoft Basic Render Driver / WARP).
// If IddCx chooses by itself it picks the Helios render adapter, and until
// Helios is fully compositable that pulls DWM/Explorer into the experimental UMD
// path and can crash-loop the desktop.
//
// Experimental (HeliosRenderAdapter == 1): select the Helios WDDM render adapter
// (PCI\VEN_1AF4&DEV_1050) so the OS composites the desktop ON Helios → venus →
// host GPU, and the IDD captures the composed frame via the standard IddCx
// swapchain. This is the goal path; gated behind the registry flag so it is
// reversible without a rebuild (flip to 0 + restart the device) given the
// crash-loop risk during bring-up.
void CIndirectDeviceContext::SelectRenderAdapter()
{
  /* Latched once per adapter lifetime. Re-issuing the preference on every
   * FinishInit/replug re-triggered render/indirect pairing re-creation each
   * cycle — a positive-feedback churn loop (replug → SetRenderAdapter →
   * pairing churn → ACCESS_LOST → replug). The preference is adapter-scoped
   * state; once issued it persists for this adapter instance. */
  if (m_renderAdapterSelected)
  {
    DEBUG_TRACE("SelectRenderAdapter: already latched for this adapter, skipping");
    return;
  }

  const bool preferHelios = g_settings.ReadBoolValue(L"HeliosRenderAdapter", false);

  IDXGIFactory * factory = NULL;
  IDXGIAdapter * dxgiAdapter;
  bool selectedRenderAdapter = false;
  if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory)))
  {
    for (UINT i = 0; factory->EnumAdapters(i, &dxgiAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
      DXGI_ADAPTER_DESC adapterDesc;
      dxgiAdapter->GetDesc(&adapterDesc);
      dxgiAdapter->Release();

      const bool isBasicRenderDriver =
        (adapterDesc.VendorId == 0x1414 && adapterDesc.DeviceId == 0x008c) ||
        wcsstr(adapterDesc.Description, L"Microsoft Basic Render Driver") != nullptr;
      const bool isHelios = (adapterDesc.VendorId == 0x1af4 && adapterDesc.DeviceId == 0x1050);

      const bool wanted = preferHelios ? isHelios : isBasicRenderDriver;

      DEBUG_INFO(L"IDD render adapter[%u]: %s vendor=0x%04x device=0x%04x luid=%08x:%08x%s",
        i,
        adapterDesc.Description,
        adapterDesc.VendorId,
        adapterDesc.DeviceId,
        (unsigned)adapterDesc.AdapterLuid.HighPart,
        (unsigned)adapterDesc.AdapterLuid.LowPart,
        wanted ? L" selected" : L" skipped");

      if (!wanted)
        continue;

      IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
      args.PreferredRenderAdapter = adapterDesc.AdapterLuid;
      /* returns void: the preference is fire-and-forget; whether the OS
       * honors it shows up in the next AssignSwapChain's renderAdapter LUID */
      IddCxAdapterSetRenderAdapter(m_adapter, &args);
      DEBUG_INFO(L"IddCxAdapterSetRenderAdapter(%s, preferHelios=%d) issued",
        adapterDesc.Description, preferHelios ? 1 : 0);
      selectedRenderAdapter = true;
      m_renderAdapterSelected = true;
      m_selectedRenderAdapterLuid = adapterDesc.AdapterLuid;
      break;
    }
    factory->Release();
  }
  else
    DEBUG_WARN("CreateDXGIFactory failed while selecting IDD render adapter");

  if (!selectedRenderAdapter)
    DEBUG_WARN("Render-adapter preference NOT registered; IddCx render adapter remains OS-selected");
}

/* The render-adapter preference latch is adapter-instance-scoped, but the
 * RENDER adapter can be PnP-restarted underneath us (every Helios UMD/KMD
 * hotplug deploy does this), minting a new LUID. The latched preference then
 * names a dead adapter and the OS can never build the swapchain pairing —
 * observed 2026-07-04 as an endless no-AssignSwapChain replug loop after a
 * deploy. Called from the watchdog only when pairing is already failing, so
 * the enumeration cost stays off the happy path. */
void CIndirectDeviceContext::RevalidateRenderAdapter()
{
  if (!m_renderAdapterSelected)
    return;

  IDXGIFactory * factory = NULL;
  if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory)))
    return;

  bool stillPresent = false;
  IDXGIAdapter * dxgiAdapter;
  for (UINT i = 0; factory->EnumAdapters(i, &dxgiAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC adapterDesc;
    dxgiAdapter->GetDesc(&adapterDesc);
    dxgiAdapter->Release();
    if (adapterDesc.AdapterLuid.HighPart == m_selectedRenderAdapterLuid.HighPart &&
        adapterDesc.AdapterLuid.LowPart  == m_selectedRenderAdapterLuid.LowPart)
    {
      stillPresent = true;
      break;
    }
  }
  factory->Release();

  if (stillPresent)
    return;

  DEBUG_WARN("Latched render adapter LUID %08x:%08x no longer exists "
             "(render adapter restarted?) — re-selecting",
             (unsigned)m_selectedRenderAdapterLuid.HighPart,
             (unsigned)m_selectedRenderAdapterLuid.LowPart);
  m_renderAdapterSelected = false;
  SelectRenderAdapter();
}

void CIndirectDeviceContext::FinishInit(UINT connectorIndex)
{
  EnterCriticalSection(&m_stateLock);
  DEBUG_INFO("FinishInit: connectorIndex=%u", connectorIndex);

  SelectRenderAdapter();

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectMonitorContextWrapper);
  /* free the heap CIndirectMonitorContext when IddCx deletes the monitor
   * object (departed monitors) — replugs are automatic now, so this leaked
   * once per replug before */
  attr.EvtCleanupCallback = [](WDFOBJECT object)
  {
    auto * w = WdfObjectGet_CIndirectMonitorContextWrapper(object);
    if (w)
      w->Cleanup();
  };

  IDDCX_MONITOR_INFO info = {};
  info.Size           = sizeof(info);
  info.MonitorType    = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = connectorIndex;

  info.MonitorDescription.Size     = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = m_edid.Size();
  info.MonitorDescription.pData    = const_cast<BYTE*>(m_edid.Data());

  CoCreateGuid(&info.MonitorContainerId);

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo     = &info;

  IDARG_OUT_MONITORCREATE createOut;
  NTSTATUS status = IddCxMonitorCreate(m_adapter, &create, &createOut);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorCreate Failed");
    /* watchdog retries FinishInit after NOMONITOR_RETRY_MS */
    EnterState(MonitorState::NoMonitor);
    LeaveCriticalSection(&m_stateLock);
    return;
  }
  DEBUG_INFO("IddCxMonitorCreate OK");

  m_monitor = createOut.MonitorObject;
  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(m_monitor);
  wrapper->context = new CIndirectMonitorContext(m_monitor, this);

  IDARG_OUT_MONITORARRIVAL out;
  status = IddCxMonitorArrival(m_monitor, &out);
  if (FAILED(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorArrival Failed");
    /* the monitor object exists but never arrived — delete it so the
     * NoMonitor retry does not pile up monitor objects */
    delete wrapper->context;
    wrapper->context = nullptr;
    WdfObjectDelete((WDFOBJECT)m_monitor);
    m_monitor = nullptr;
    EnterState(MonitorState::NoMonitor);
    LeaveCriticalSection(&m_stateLock);
    return;
  }
  DEBUG_INFO("IddCxMonitorArrival OK");

  /* waiting for AssignSwapChain; watchdog replugs after OFFER_TIMEOUT_MS */
  InterlockedExchange(&m_unassignSeen, 0);
  InterlockedExchange(&m_firstFrameSeen, 0);
  EnterState(MonitorState::Arrived);
  LeaveCriticalSection(&m_stateLock);
}

/* The replug primitive. ALWAYS completes: departure moves the machine to
 * ReplugPending, and the watchdog re-runs FinishInit either when the OS
 * confirms via UnassignSwapChain or unconditionally after REPLUG_SETTLE_MS.
 * (The old implementation re-armed only from OnUnassignedSwapChain, which the
 * OS never calls when no swapchain was assigned at departure time — the
 * monitor then never returned and every future replug no-op'd.) */
void CIndirectDeviceContext::ReplugMonitor()
{
  /* NEVER depart a monitor while an AssignSwapChain callback is executing or
   * the processor thread is inside IddCxSwapChainSetDevice: IddCx then tears
   * down its swapchain/monitor objects under the running call, corrupting its
   * internal lists — FAST_FAIL_CORRUPT_LIST_ENTRY (0xc0000409, data=7) at
   * iddcx.dll+0xa631, WUDFHost terminated, devnode FAILED_POST_START
   * (boot #4, 2026-07-03). Defer instead: every caller class self-retries
   * (the watchdog re-evaluates its condition on the next tick; LGMP-queued
   * requests are superseded by the very assignment that is in flight). */
  if (InterlockedCompareExchange(&m_assignInFlight, 0, 0) ||
      InterlockedCompareExchange(&m_bindingInFlight, 0, 0))
  {
    DEBUG_INFO("ReplugMonitor deferred: swapchain assignment/binding in flight");
    return;
  }

  /* NEVER depart a monitor while the processor thread holds an acquired
   * frame: departure destroys the child swapchain with the frame outstanding
   * and IddCx bugchecks the host process for the unreleased frame
   * (ReportBugcheckForSwapChainTimeoutDriverDidNotReleaseFrame — observed
   * 2026-07-03 10:25:52Z, dump WUDFHost.exe-(PID-8464), self-recovered).
   * The hold window (acquire → FinishedProcessingFrame) is tiny; re-queue so
   * the LGMP timer retries next tick (ProcessLGMP consumed the queued flag
   * before calling here), and watchdog-driven callers re-evaluate on their
   * own tick. Like the gates above this narrows the race to instants rather
   * than eliminating it — IddCx itself surfaces a departure-vs-acquire
   * collision as ACCESS_LOST on the acquire, which escalates to replug. */
  if (InterlockedCompareExchange(&m_frameHeld, 0, 0))
  {
    DEBUG_INFO("ReplugMonitor deferred: acquired frame in flight");
    InterlockedExchange(&m_replugMonitorQueued, 1);
    return;
  }

  EnterCriticalSection(&m_stateLock);

  if ((MonitorState)m_monitorState == MonitorState::ReplugPending)
  {
    LeaveCriticalSection(&m_stateLock);
    return;
  }

  if (m_monitor == WDF_NO_HANDLE)
  {
    LeaveCriticalSection(&m_stateLock);
    FinishInit(0);
    return;
  }

  DEBUG_TRACE("ReplugMonitor");
  InterlockedExchange(&m_unassignSeen, 0);
  NTSTATUS status = IddCxMonitorDeparture(m_monitor);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR("IddCxMonitorDeparture Failed (0x%08x)", status);
    /* monitor presumably still live — fall back to Arrived so the
     * offer-timeout watchdog retries the replug later instead of stalling */
    EnterState(MonitorState::Arrived);
    LeaveCriticalSection(&m_stateLock);
    return;
  }

  /* IddCx owns the departed monitor object's teardown; FinishInit creates a
   * fresh one and overwrites m_monitor */
  EnterState(MonitorState::ReplugPending);
  LeaveCriticalSection(&m_stateLock);
}

/* Runs every WATCHDOG_PERIOD_MS from its own WDF timer (alive from adapter
 * init, unlike the LGMP timer which only exists after the first swapchain).
 * This is the single place that drives recovery transitions, so recovery does
 * not depend on any OS callback that may never come. */
void CIndirectDeviceContext::MonitorWatchdog()
{
  if (!m_adapter)
    return;

  /* skip if a previous invocation is still running (FinishInit can block) */
  if (InterlockedCompareExchange(&m_watchdogActive, 1, 0) != 0)
    return;

  EnterCriticalSection(&m_stateLock);
  const ULONGLONG elapsed = GetTickCount64() - m_stateEnteredTick;

  switch ((MonitorState)m_monitorState)
  {
    case MonitorState::NoMonitor:
      if (elapsed >= NOMONITOR_RETRY_MS)
      {
        DEBUG_WARN("Watchdog: no monitor for %llums, re-running FinishInit", elapsed);
        FinishInit(0);
      }
      break;

    case MonitorState::Arrived:
    {
      const LONG fruitless = InterlockedCompareExchange(&m_fruitlessReplugs, 0, 0);
      const ULONGLONG offerWindow =
        OFFER_TIMEOUT_MS << min(fruitless, OFFER_BACKOFF_MAX_SHIFT);
      if (elapsed >= offerWindow)
      {
        DEBUG_WARN("Watchdog: monitor arrived but no AssignSwapChain within %llums "
                   "(window %llums, fruitless replugs %ld), replugging",
                   elapsed, offerWindow, fruitless);
        /* a fruitless offer often means the latched render adapter LUID died
         * (Helios PnP restart) — re-issue the preference before replugging */
        RevalidateRenderAdapter();
        InterlockedIncrement(&m_fruitlessReplugs);
        ReplugMonitor();
      }
      break;
    }

    case MonitorState::SwapChainBound:
      if (!InterlockedCompareExchange(&m_firstFrameSeen, 0, 0))
      {
        /* NotifyDevicesReady refreshes m_stateEnteredTick, so `elapsed` counts
         * from device-ready once the processor's D3D bring-up completed; until
         * then only the absolute init cap applies. */
        const bool devicesReady =
          InterlockedCompareExchange(&m_devicesReady, 0, 0) != 0;
        if (!devicesReady && elapsed >= DEVICEINIT_TIMEOUT_MS)
        {
          DEBUG_WARN("Watchdog: swapchain bound but device bring-up incomplete after "
                     "%llums, replugging", elapsed);
          InterlockedIncrement(&m_fruitlessReplugs);
          ReplugMonitor();
        }
        else if (devicesReady && elapsed >= FIRSTFRAME_TIMEOUT_MS)
        {
          DEBUG_WARN("Watchdog: devices ready but zero frames acquired within %llums "
                     "(stale binding), replugging", elapsed);
          InterlockedIncrement(&m_fruitlessReplugs);
          ReplugMonitor();
        }
      }
      else
      {
        /* mid-stream stall DETECTION ONLY: an idle desktop legitimately
         * presents nothing for minutes, so zero acquires is NOT a reliable
         * failure signal from this side and auto-replugging here destroys
         * healthy bindings (false-positived live 2026-07-04 within minutes
         * of deploy). The actual mid-stream stall causes are handled where
         * they are observable: a wedged dwm now gets DEVICE_LOST from the
         * ICD's forward-progress deadline and restarts itself, and an
         * abandoned swapchain surfaces as ACCESS_LOST in the processor
         * (OnSwapChainDeviceLost → replug). Keep the loud breadcrumb (once
         * per binding) so a genuine unrecovered stall class shows up in the
         * log with timing evidence. */
        const LONGLONG lastAcquire =
          InterlockedCompareExchange64(&m_lastAcquireTick, 0, 0);
        const ULONGLONG now = GetTickCount64();
        if (!InterlockedCompareExchange(&m_midstreamReplugged, 0, 0) &&
            lastAcquire > 0 && now - (ULONGLONG)lastAcquire >= MIDSTREAM_STALL_MS)
        {
          DEBUG_WARN("Watchdog: no acquires for %llums on a previously-flowing binding "
                     "(idle desktop OR a genuine stall; not replugging)",
                     now - (ULONGLONG)lastAcquire);
          InterlockedExchange(&m_midstreamReplugged, 1);
        }
      }
      break;

    case MonitorState::ReplugPending:
      if (InterlockedCompareExchange(&m_unassignSeen, 0, 0) ||
          elapsed >= REPLUG_SETTLE_MS)
      {
        DEBUG_INFO("Watchdog: completing replug (%s after %llums)",
          m_unassignSeen ? "unassign confirmed" : "forced", elapsed);
        FinishInit(0);
      }
      break;
  }

  LeaveCriticalSection(&m_stateLock);
  InterlockedExchange(&m_watchdogActive, 0);
}

void CIndirectDeviceContext::OnAssignSwapChain()
{
  InterlockedExchange(&m_recoverModeUpdateSwapChain, 0);
  /* a fresh assignment supersedes any queued replug */
  InterlockedExchange(&m_replugMonitorQueued, 0);

  EnterCriticalSection(&m_stateLock);
  InterlockedExchange(&m_firstFrameSeen, 0);
  InterlockedExchange(&m_devicesReady, 0);
  InterlockedExchange(&m_midstreamReplugged, 0);
  EnterState(MonitorState::SwapChainBound);
  LeaveCriticalSection(&m_stateLock);

  if (m_doSetMode)
  {
    m_doSetMode = false;
    g_pipe.SetDisplayMode(m_setMode.width, m_setMode.height, m_setMode.refresh);
  }
}

void CIndirectDeviceContext::OnUnassignedSwapChain()
{
  InterlockedExchange(&m_replugMonitorQueued, 0);
  InterlockedExchange(&m_recoverModeUpdateSwapChain, 0);
  InterlockedExchange(&m_unassignSeen, 1);

  EnterCriticalSection(&m_stateLock);
  if ((MonitorState)m_monitorState != MonitorState::ReplugPending)
  {
    /* OS-initiated unassign (topology change): the monitor is still live and
     * a re-offer should follow — wait in Arrived; if the OS never re-offers,
     * the offer-timeout watchdog replugs */
    EnterState(MonitorState::Arrived);
  }
  /* if a replug is pending, the watchdog completes it (unassignSeen fast
   * path) — do NOT FinishInit from this callback: the forced-timeout path
   * must remain the single re-arrival driver alongside it, and running
   * FinishInit on the watchdog thread keeps re-arrival single-threaded */
  LeaveCriticalSection(&m_stateLock);
}

void CIndirectDeviceContext::NotifyFrameAcquired()
{
  InterlockedExchange(&m_firstFrameSeen, 1);
  InterlockedExchange64(&m_lastAcquireTick, (LONGLONG)GetTickCount64());
  /* a real frame is the only true success signal: reset the replug backoff
   * and re-arm the (once-per-binding) mid-stream stall watchdog */
  InterlockedExchange(&m_fruitlessReplugs, 0);
  InterlockedExchange(&m_midstreamReplugged, 0);
}

void CIndirectDeviceContext::NotifyDevicesReady()
{
  InterlockedExchange(&m_devicesReady, 1);
  /* start the first-frame window from device-ready, not from the bind: the
   * processor's D3D bring-up can take 10-15 s under venus churn and the
   * watchdog must not count that against the binding */
  EnterCriticalSection(&m_stateLock);
  if ((MonitorState)m_monitorState == MonitorState::SwapChainBound)
    m_stateEnteredTick = GetTickCount64();
  LeaveCriticalSection(&m_stateLock);
}

void CIndirectDeviceContext::NotifyFrameHeld()
{
  InterlockedIncrement(&m_frameHeld);
}

void CIndirectDeviceContext::NotifyFrameReleased()
{
  InterlockedDecrement(&m_frameHeld);
}

void CIndirectDeviceContext::NotifySwapChainSelfExit()
{
  if (InterlockedExchange(&m_replugMonitorQueued, 1))
    return;

  DEBUG_WARN("Swapchain processor exited without a terminate request, queueing monitor replug");
}

void CIndirectDeviceContext::AssignCallbackEnter()
{
  InterlockedExchange(&m_assignInFlight, 1);
}

void CIndirectDeviceContext::AssignCallbackExit()
{
  InterlockedExchange(&m_assignInFlight, 0);
}

void CIndirectDeviceContext::NotifyBindingStart()
{
  InterlockedExchange(&m_bindingInFlight, 1);
}

void CIndirectDeviceContext::NotifyBindingDone()
{
  InterlockedExchange(&m_bindingInFlight, 0);

  /* the stale-binding (first-frame) timeout measures from the moment the
   * device was bound to the swapchain — binding attempts themselves can
   * legitimately take tens of seconds during boot pairing churn */
  EnterCriticalSection(&m_stateLock);
  if ((MonitorState)m_monitorState == MonitorState::SwapChainBound)
    m_stateEnteredTick = GetTickCount64();
  LeaveCriticalSection(&m_stateLock);
}

void CIndirectDeviceContext::OnSwapChainLost()
{
  // A mode update normally keeps the swap chain alive. If Windows instead
  // reports the existing path disappeared before we see a frame at the new
  // size, recover by scheduling the old replug path from the LGMP timer so we
  // do not tear down the swap chain from one of its worker threads.
  if (!InterlockedCompareExchange(&m_recoverModeUpdateSwapChain, 0, 0))
    return;

  if (InterlockedExchange(&m_replugMonitorQueued, 1))
    return;

  DEBUG_WARN("Swap chain was lost after a mode update, falling back to monitor replug");
}

void CIndirectDeviceContext::OnSwapChainDeviceLost()
{
  // IddCx/DXGI can report DXGI_ERROR_ACCESS_LOST when the swapchain's keyed
  // mutex is abandoned. Queue a monitor replug from the LGMP timer instead of
  // tearing down IddCx objects from the swapchain worker thread.
  if (InterlockedExchange(&m_replugMonitorQueued, 1))
    return;

  DEBUG_WARN("Swap chain device/access was lost, queueing monitor replug");
}

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO & mode, DWORD width, DWORD height, DWORD vsync, bool monitorMode)
{
  mode.totalSize.cx = mode.activeSize.cx = width;
  mode.totalSize.cy = mode.activeSize.cy = height;

  mode.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  mode.AdditionalSignalInfo.videoStandard    = 255;
  
  mode.vSyncFreq.Numerator   = vsync;
  mode.vSyncFreq.Denominator = 1;
  mode.hSyncFreq.Numerator   = vsync * height;
  mode.hSyncFreq.Denominator = 1;

  mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  mode.pixelRate        = ((UINT64)vsync) * ((UINT64)width) * ((UINT64)height);
}

NTSTATUS CIndirectDeviceContext::ParseMonitorDescription(
  const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  outArgs->MonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorGetDefaultModes(
  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs)
{
  outArgs->DefaultMonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->DefaultMonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->DefaultMonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pDefaultMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
      (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorQueryTargetModes(
  const IDARG_IN_QUERYTARGETMODES* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  outArgs->TargetModeBufferOutputCount = (UINT)m_displayModes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->TargetModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pTargetModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_TARGET_MODE);
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
  }

  return STATUS_SUCCESS;
}


#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
NTSTATUS CIndirectDeviceContext::ParseMonitorDescription2(
  const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  outArgs->MonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size = sizeof(IDDCX_MONITOR_MODE2);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);
    mode->BitsPerComponent = GetWireBitsPerComponent(CanUseIddCx110DDIs());

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorQueryTargetModes2(
  const IDARG_IN_QUERYTARGETMODES2* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  outArgs->TargetModeBufferOutputCount = (UINT)m_displayModes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)m_displayModes.size())
    return STATUS_SUCCESS;

  if (!inArgs->pTargetModes)
    return STATUS_INVALID_PARAMETER;

  auto* mode = inArgs->pTargetModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size = sizeof(IDDCX_TARGET_MODE2);
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
    mode->BitsPerComponent = GetWireBitsPerComponent(CanUseIddCx110DDIs());
  }

  return STATUS_SUCCESS;
}
#endif

bool CIndirectDeviceContext::UpdateMonitorModes()
{
  if (!m_monitor)
    return false;

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  if (CanUseIddCx110DDIs())
  {
    IDDCX_TARGET_MODE2* modes = (IDDCX_TARGET_MODE2*)_malloca(
      m_displayModes.size() * sizeof(IDDCX_TARGET_MODE2));
    if (!modes)
    {
      DEBUG_WARN("Failed to allocate memory for the mode list");
      return false;
    }

    ZeroMemory(modes, m_displayModes.size() * sizeof(IDDCX_TARGET_MODE2));

    auto* mode = modes;
    for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
    {
      mode->Size              = sizeof(IDDCX_TARGET_MODE2);
      mode->RequiredBandwidth = (UINT64)it->width * it->height * it->refresh * 32;
      mode->BitsPerComponent  = GetWireBitsPerComponent(CanUseIddCx110DDIs());
      FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
    }

    IDARG_IN_UPDATEMODES2 updateModes = {};
    updateModes.Reason          = IDDCX_UPDATE_REASON_OTHER;
    updateModes.TargetModeCount = (UINT)m_displayModes.size();
    updateModes.pTargetModes    = modes;

    NTSTATUS status = IddCxMonitorUpdateModes2(m_monitor, &updateModes);
    _freea(modes);
    if (!NT_SUCCESS(status))
    {
      DEBUG_WARN("IddCxMonitorUpdateModes2 Failed (0x%08x)", status);
      return false;
    }

    return true;
  }
#endif

  IDDCX_TARGET_MODE* modes = (IDDCX_TARGET_MODE*)_malloca(
    m_displayModes.size() * sizeof(IDDCX_TARGET_MODE));
  if (!modes)
  {
    DEBUG_WARN("Failed to allocate memory for the mode list");
    return false;
  }

  ZeroMemory(modes, m_displayModes.size() * sizeof(IDDCX_TARGET_MODE));

  auto* mode = modes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size              = sizeof(IDDCX_TARGET_MODE);
    mode->RequiredBandwidth = (UINT64)it->width * it->height * it->refresh * 32;
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
  }

  IDARG_IN_UPDATEMODES updateModes = {};
  updateModes.Reason          = IDDCX_UPDATE_REASON_OTHER;
  updateModes.TargetModeCount = (UINT)m_displayModes.size();
  updateModes.pTargetModes    = modes;

  NTSTATUS status = IddCxMonitorUpdateModes(m_monitor, &updateModes);
  _freea(modes);
  if (!NT_SUCCESS(status))
  {
    DEBUG_WARN("IddCxMonitorUpdateModes Failed (0x%08x)", status);
    return false;
  }

  return true;
}

void CIndirectDeviceContext::SetResolution(int width, int height)
{
  m_setMode.width     = width;
  m_setMode.height    = height;
  m_setMode.refresh   = g_settings.GetDefaultRefresh();
  m_setMode.preferred = true;
  g_settings.SetExtraMode(m_setMode);

  PopulateDefaultModes();

  if (UpdateMonitorModes())
  {
    DEBUG_TRACE("Updated monitor modes without replugging");
    m_doSetMode = false;
    InterlockedExchange(&m_recoverModeUpdateSwapChain, 1);
    g_pipe.SetDisplayMode(m_setMode.width, m_setMode.height, m_setMode.refresh);
    return;
  }

  DEBUG_TRACE("Falling back to monitor replug for mode update");
  m_doSetMode = true;
  InterlockedExchange(&m_recoverModeUpdateSwapChain, 0);
  ReplugMonitor();
}

bool CIndirectDeviceContext::SetupLGMP(size_t alignSize)
{
  // this may get called multiple times as we need to delay calling it until
  // we can determine the required alignment from the GPU in use
  if (m_lgmp)
    return true;

  m_alignSize = alignSize;
  
  std::stringstream ss;
  {
    KVMFR kvmfr = {};
    memcpy_s(kvmfr.magic, sizeof(kvmfr.magic), KVMFR_MAGIC, sizeof(KVMFR_MAGIC) - 1);
    kvmfr.version  = KVMFR_VERSION;
    kvmfr.features =
      KVMFR_FEATURE_SETCURSORPOS |
      KVMFR_FEATURE_WINDOWSIZE;
    strncpy_s(kvmfr.hostver, LG_VERSION_STR, sizeof(kvmfr.hostver) - 1);
    ss.write(reinterpret_cast<const char *>(&kvmfr), sizeof(kvmfr));
  }

  {
    const std::string & model = CPlatformInfo::GetCPUModel();

    KVMFRRecord_VMInfo * vmInfo = static_cast<KVMFRRecord_VMInfo *>(calloc(1, sizeof(*vmInfo)));
    if (!vmInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_VMInfo");
      return false;
    }
    vmInfo->cpus    = static_cast<uint8_t>(CPlatformInfo::GetProcCount  ());
    vmInfo->cores   = static_cast<uint8_t>(CPlatformInfo::GetCoreCount  ());
    vmInfo->sockets = static_cast<uint8_t>(CPlatformInfo::GetSocketCount());

    const uint8_t * uuid = CPlatformInfo::GetUUID();
    memcpy_s (vmInfo->uuid, sizeof(vmInfo->uuid), uuid, 16);
    strncpy_s(vmInfo->capture, "Looking Glass IDD Driver", sizeof(vmInfo->capture));

    KVMFRRecord * record = static_cast<KVMFRRecord *>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_VMINFO;
    record->size = sizeof(*vmInfo) + (uint32_t)model.length() + 1;

    ss.write(reinterpret_cast<const char*>(record       ), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(vmInfo       ), sizeof(*vmInfo));
    ss.write(reinterpret_cast<const char*>(model.c_str()), model.length() + 1);
  }

  {
    KVMFRRecord_OSInfo * osInfo = static_cast<KVMFRRecord_OSInfo *>(calloc(1, sizeof(*osInfo)));
    if (!osInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_OSInfo");
      return false;
    }

    osInfo->os = KVMFR_OS_WINDOWS;

    const std::string & osName = CPlatformInfo::GetProductName();

    KVMFRRecord* record = static_cast<KVMFRRecord*>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_OSINFO;
    record->size = sizeof(*osInfo) + (uint32_t)osName.length() + 1;

    ss.write(reinterpret_cast<const char*>(record), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(osInfo), sizeof(*osInfo));
    ss.write(reinterpret_cast<const char*>(osName.c_str()), osName.length() + 1);
  }

  LGMP_STATUS status;
  std::string udata = ss.str();

  if ((status = lgmpHostInit(m_ivshmem.GetMem(), (uint32_t)m_ivshmem.GetSize(),
    &m_lgmp, (uint32_t)udata.size(), (uint8_t*)&udata[0])) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit Failed: %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, FRAME_QUEUE_CONFIG, &m_frameQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame): %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, POINTER_QUEUE_CONFIG, &m_pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Pointer): %s", lgmpStatusString(status));
    return false;
  }

  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerMemory[i]), 0, MAX_POINTER_SIZE);
  }

  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerShapeMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Shapes): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerShapeMemory[i]), 0, MAX_POINTER_SIZE);
  }

  const size_t availFrameMem = lgmpHostMemAvail(m_lgmp);
  m_maxFrameSize = availFrameMem;
  m_maxFrameSize = (m_maxFrameSize -(m_alignSize - 1)) & ~(m_alignSize - 1);
  m_maxFrameSize /= LGMP_Q_FRAME_LEN;
  DEBUG_INFO("Max Frame Size: %u MiB", (unsigned int)(m_maxFrameSize / 1048576LL));

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(m_lgmp, (uint32_t)m_maxFrameSize,
        (uint32_t)m_alignSize, &m_frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAllocAligned Failed (Frame): %s", lgmpStatusString(status));
      return false;
    }

    m_frame[i] = (KVMFRFrame *)lgmpHostMemPtr(m_frameMemory[i]);

    /**
     * put the framebuffer on the border of the next page, this is to allow for
     * aligned DMA tranfers by the reciever */
    const size_t alignOffset = alignSize - sizeof(FrameBuffer);
    m_frame[i]->offset = (uint32_t)alignOffset;
    m_frameBuffer[i] = (FrameBuffer*)(((uint8_t*)m_frame[i]) + alignOffset);
  }

  WDF_TIMER_CONFIG config;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&config,
    [](WDFTIMER timer) -> void
    {
      WDFOBJECT parent = WdfTimerGetParentObject(timer);
      auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(parent);
      wrapper->context->LGMPTimer();
    },
    10);
  config.AutomaticSerialization = FALSE;

  /**
  * documentation states that Dispatch is not available under the UDMF, however...
  * using Passive returns a not supported error, and Dispatch works.
  */
  WDF_OBJECT_ATTRIBUTES attribs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
  attribs.ParentObject   = m_wdfDevice;
  attribs.ExecutionLevel = WdfExecutionLevelDispatch;

  NTSTATUS s = WdfTimerCreate(&config, &attribs, &m_lgmpTimer);
  if (!NT_SUCCESS(s))
  {
    DEBUG_ERROR_HR(s, "Timer creation failed");
    return false;
  }
  WdfTimerStart(m_lgmpTimer, WDF_REL_TIMEOUT_IN_MS(10));

  return true;
}

void CIndirectDeviceContext::DeInitLGMP()
{
  m_hasFrame = false;

  if (m_lgmp == nullptr)
    return;

  if (m_lgmpTimer)
  {
    WdfTimerStop(m_lgmpTimer, TRUE);
    m_lgmpTimer = nullptr;
  }

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmpHostMemFree(&m_frameMemory[i]);
  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&m_pointerMemory[i]);
  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerShapeMemory[i]);
  lgmpHostFree(&m_lgmp);
}

void CIndirectDeviceContext::LGMPTimer()
{
  ProcessLGMP(true);
}

void CIndirectDeviceContext::ProcessLGMP(bool allowReplug)
{
  if (!m_lgmp)
    return;

  if (InterlockedExchange(&m_replugMonitorQueued, 0))
  {
    if (!allowReplug)
    {
      InterlockedExchange(&m_replugMonitorQueued, 1);
      return;
    }

    m_doSetMode = true;
    ReplugMonitor();
    return;
  }

  if (InterlockedCompareExchange(&m_lgmpProcessActive, 1, 0) != 0)
    return;

  LGMP_STATUS status;
  if ((status = lgmpHostProcess(m_lgmp)) != LGMP_OK)
  {
    if (status == LGMP_ERR_CORRUPTED)
    {
      DEBUG_WARN("LGMP reported the shared memory has been corrupted, attempting to recover\n");
      //TODO: fixme - reinit
      InterlockedExchange(&m_lgmpProcessActive, 0);
      return;
    }

    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));
    //TODO: fixme - shutdown
    InterlockedExchange(&m_lgmpProcessActive, 0);
    return;
  }

  uint8_t data[LGMP_MSGS_SIZE];
  size_t  size;
  while ((status = lgmpHostReadData(m_pointerQueue, &data, &size)) == LGMP_OK)
  {
    KVMFRMessage * msg = (KVMFRMessage *)data;
    switch (msg->type)
    {
      case KVMFR_MESSAGE_SETCURSORPOS:
      {
        KVMFRSetCursorPos* sp = (KVMFRSetCursorPos*)msg;
        g_pipe.SetCursorPos(sp->x, sp->y);
        break;
      }

      case KVMFR_MESSAGE_WINDOWSIZE:
      {
        KVMFRWindowSize* ws = (KVMFRWindowSize*)msg;
        SetResolution(ws->w, ws->h);
      }
    }

    lgmpHostAckData(m_pointerQueue);
  }

  if (lgmpHostQueueNewSubs(m_frameQueue) && m_monitor)
  {
    if (m_hasFrame)
      lgmpHostQueuePost(m_frameQueue, 0, m_frameMemory[m_frameIndex]);
  }

  if (lgmpHostQueueNewSubs(m_pointerQueue))
    ResendCursor();

  InterlockedExchange(&m_lgmpProcessActive, 0);
}

CIndirectDeviceContext::PreparedFrameBuffer CIndirectDeviceContext::PrepareFrameBuffer(
  unsigned pitch, const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
  const RECT * dirtyRects, unsigned nbDirtyRects)
{
  PreparedFrameBuffer result = {};
  EnterCriticalSection(&m_frameLock);

  if (!m_lgmp || !m_frameQueue)
    goto out;

  ProcessLGMP(false);

  if (InterlockedCompareExchange(&m_recoverModeUpdateSwapChain, 0, 0) &&
      srcFormat.width == m_setMode.width && srcFormat.height == m_setMode.height)
    InterlockedExchange(&m_recoverModeUpdateSwapChain, 0);

  if (m_width     != dstFormat.desc.Width  ||
      m_height    != dstFormat.desc.Height ||
      m_pitch     != pitch                 ||
      m_format    != dstFormat.desc.Format ||
      m_frameType != dstFormat.format)
  {
    m_width     = (unsigned)dstFormat.desc.Width;
    m_height    = dstFormat.desc.Height;
    m_format    = dstFormat.desc.Format;
    m_frameType = dstFormat.format;
    m_pitch     = pitch;
    ++m_formatVer;
  }

  if (++m_frameIndex == LGMP_Q_FRAME_LEN)
    m_frameIndex = 0;

  KVMFRFrame * fi = m_frame[m_frameIndex];

  // If the client falls behind, do not wedge the IDD swapchain thread forever.
  // Keep processing LGMP briefly in case the queue can be drained, then drop this
  // frame and let IddCx continue. The next frame will carry fresh damage.
  unsigned queueWaits = 0;
  while (lgmpHostQueuePending(m_frameQueue) == LGMP_Q_FRAME_LEN)
  {
    ProcessLGMP(false);
    if (++queueWaits >= 32)
    {
      const LONG drops = InterlockedIncrement(&m_frameDropCount);
      if (drops == 1 || (drops % 300) == 0)
        DEBUG_WARN("LGMP frame queue full, dropping frame (drops=%ld)", drops);
      goto out;
    }
    Sleep(0);
  }

  if (dstFormat.format == FRAME_TYPE_INVALID)
  {
    DEBUG_ERROR("Unsupported frame format, skipping frame");
    goto out;
  }

  const unsigned maxRows = (unsigned)(m_maxFrameSize / pitch);
  const int bpp = dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
  KVMFRFrameFlags flags =
    (dstFormat.hdr   ? FRAME_FLAG_HDR    : 0) |
    (dstFormat.hdrPQ ? FRAME_FLAG_HDR_PQ : 0);

  if (dstFormat.format == FRAME_TYPE_RGBA16F)
    flags |= FRAME_FLAG_HDR;

  if (maxRows < dstFormat.desc.Height)
    flags |= FRAME_FLAG_TRUNCATED;

  fi->formatVer        = m_formatVer;
  fi->frameSerial      = m_frameSerial++;
  fi->screenWidth      = srcFormat.width;
  fi->screenHeight     = srcFormat.height;
  fi->dataWidth        = (unsigned)dstFormat.desc.Width;
  fi->dataHeight       = min(maxRows, dstFormat.desc.Height);
  fi->frameWidth       = dstFormat.width;
  fi->frameHeight      = dstFormat.height;
  fi->stride           = pitch / bpp;
  fi->pitch            = pitch;
  // fi->offset is initialized at startup
  fi->flags            = flags;
  fi->rotation         = FRAME_ROT_0;
  fi->type             = dstFormat.format;
  fi->damageRectsCount = 0;
  if (nbDirtyRects <= ARRAYSIZE(fi->damageRects))
  {
    fi->damageRectsCount = nbDirtyRects;
    for (unsigned i = 0; i < nbDirtyRects; ++i)
    {
      fi->damageRects[i].x      = dirtyRects[i].left;
      fi->damageRects[i].y      = dirtyRects[i].top;
      fi->damageRects[i].width  = dirtyRects[i].right  - dirtyRects[i].left;
      fi->damageRects[i].height = dirtyRects[i].bottom - dirtyRects[i].top;
    }
  }

  FrameBuffer* fb = m_frameBuffer[m_frameIndex];
  fb->wp = 0;

  LGMP_STATUS status = lgmpHostQueuePost(m_frameQueue, 0, m_frameMemory[m_frameIndex]);
  if (status != LGMP_OK)
  {
    const LONG drops = InterlockedIncrement(&m_frameDropCount);
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      if (drops == 1 || (drops % 300) == 0)
        DEBUG_WARN("LGMP frame queue full on post, dropping frame (drops=%ld)", drops);
    }
    else
      DEBUG_ERROR("lgmpHostQueuePost Failed (Frame): %s", lgmpStatusString(status));
    goto out;
  }
  ProcessLGMP(false);

  result.frameIndex = m_frameIndex;
  result.mem        = fb->data;
  result.pitch      = pitch;
  result.height     = fi->dataHeight;

  m_hasFrame = true;
out:
  LeaveCriticalSection(&m_frameLock);
  return result;
}

void CIndirectDeviceContext::WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos) const
{
  FrameBuffer * fb = m_frameBuffer[frameIndex];

  memcpy(
    (void *)((uintptr_t)fb->data + offset),
    (void *)((uintptr_t)src + offset),
    len);

  if (setWritePos)
    fb->wp = (uint32_t)(offset + len);
}

void CIndirectDeviceContext::FinalizeFrameBuffer(unsigned frameIndex, size_t len) const
{
  FrameBuffer * fb = m_frameBuffer[frameIndex];
  fb->wp = (uint32_t)len;
}

void CIndirectDeviceContext::FinalizeFrameBuffer(unsigned frameIndex) const
{
  FinalizeFrameBuffer(frameIndex, (size_t)m_height * m_pitch);
}

void CIndirectDeviceContext::PresentHeliosFrame(unsigned frameIndex)
{
  (void)frameIndex;
}

void CIndirectDeviceContext::SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info, const BYTE * data)
{
  if (!m_lgmp || !m_pointerQueue)
    return;

  PLGMPMemory mem;
  if (info.CursorShapeInfo.CursorType == IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    mem = m_pointerMemory[m_pointerMemoryIndex];
    if (++m_pointerMemoryIndex == LGMP_Q_POINTER_LEN)
      m_pointerMemoryIndex = 0;
  }
  else
  {
    mem = m_pointerShapeMemory[m_pointerShapeIndex];
    if (++m_pointerShapeIndex == POINTER_SHAPE_BUFFERS)
      m_pointerShapeIndex = 0;
  }

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  
  m_cursorVisible = info.IsCursorVisible;
  uint32_t flags  = 0;

  if (info.IsCursorVisible)
  {
    m_cursorX       = info.X;
    m_cursorY       = info.Y;
    cursor->x = (int16_t)info.X;
    cursor->y = (int16_t)info.Y;
    flags |= CURSOR_FLAG_POSITION | CURSOR_FLAG_VISIBLE;
  }

  if (info.CursorShapeInfo.CursorType != IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    memcpy(cursor + 1, data,
      (size_t)info.CursorShapeInfo.Height * info.CursorShapeInfo.Pitch);

    cursor->hx     = (int8_t  )info.CursorShapeInfo.XHot;
    cursor->hy     = (int8_t  )info.CursorShapeInfo.YHot;
    cursor->width  = (uint32_t)info.CursorShapeInfo.Width;
    cursor->height = (uint32_t)info.CursorShapeInfo.Height;
    cursor->pitch  = (uint32_t)info.CursorShapeInfo.Pitch;

    switch (info.CursorShapeInfo.CursorType)
    {
      case IDDCX_CURSOR_SHAPE_TYPE_ALPHA:
        cursor->type = CURSOR_TYPE_COLOR;
        break;

      case IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR:
        cursor->type = CURSOR_TYPE_MASKED_COLOR;
        break;
    }

    flags |= CURSOR_FLAG_SHAPE;
    m_pointerShape = mem;
  }

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    break;
  }
}

void CIndirectDeviceContext::ResendCursor() const
{
  PLGMPMemory mem = m_pointerShape;
  if (!mem)
    return;

  KVMFRCursor* cursor = (KVMFRCursor*)lgmpHostMemPtr(mem);
  cursor->x = (int16_t)m_cursorX;
  cursor->y = (int16_t)m_cursorY;

  const uint32_t flags =
    CURSOR_FLAG_POSITION | CURSOR_FLAG_SHAPE |
    (m_cursorVisible ? CURSOR_FLAG_VISIBLE : 0);

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    break;
  }
}
