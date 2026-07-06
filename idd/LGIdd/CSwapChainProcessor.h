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

#include "CD3D11Device.h"
#include "CD3D12Device.h"
#include "CIndirectDeviceContext.h"
#include "CInteropResourcePool.h"
#include "CFrameBufferPool.h"
#include "CPostProcessor.h"

#include <Windows.h>
#include <wrl.h>
#include <IddCx.h>
#include <memory>

using namespace Microsoft::WRL;

#define STAGING_TEXTURES 3

/* Lifetime model: shared_ptr-managed. Each processing thread holds its own
 * reference (handed over at CreateThread), and the monitor context holds one.
 * Stop() only SIGNALS termination — it never joins: a processing thread can
 * be blocked inside D3D11CreateDevice or IddCxSwapChainSetDevice for tens of
 * seconds during boot/restart pairing churn, and IddCx's watchdog TERMINATES
 * WUDFHost when a DDI callback (Assign/UnassignSwapChain) overruns
 * (IddWatchdog::WatchdogThread -> IddAdapter::ReportBugcheck ->
 * TerminateProcess; full-memory dump 2026-07-03 09:07). The last exiting
 * thread drops the last reference and the destructor — which therefore never
 * has live threads to wait for — deletes the swapchain object. */
class CSwapChainProcessor : public std::enable_shared_from_this<CSwapChainProcessor>
{
private:
  IDDCX_MONITOR                   m_monitor;
  CIndirectDeviceContext        * m_devContext;
  IDDCX_SWAPCHAIN                 m_hSwapChain;
  LUID                            m_renderAdapter;
  std::shared_ptr<CD3D11Device>   m_dx11Device;
  std::shared_ptr<CD3D12Device>   m_dx12Device;
  HANDLE                          m_newFrameEvent;
  bool                            m_swapChainDeviceSet = false;

  CInteropResourcePool m_resPool;
  CFrameBufferPool     m_fbPool;
  CPostProcessor       m_postProcessor;

  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_thread[2];
  Wrappers::Event m_terminateEvent;

  Wrappers::Event m_cursorDataEvent;
  BYTE*           m_shapeBuffer;
  DWORD           m_lastShapeId = 0;

  RECT     m_dirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned m_nbDirtyRects = 0;
  /* HKLM\SOFTWARE\LookingGlass\IDD!HeliosForceFullDamage — when set, every
   * acquired frame is treated as fully damaged: full-frame GPU copy AND
   * full-frame KVMFR damage, bypassing IddCx's cumulative per-buffer dirty
   * rects entirely. Diagnostic lever for ghosting suspected to be dirty-rect
   * misattribution (Helios flip-identity rotation vs dxgkrnl bookkeeping);
   * read once per swapchain bring-up (InitDevices) — toggle takes effect on
   * IDD device restart / monitor replug. */
  bool     m_forceFullDamage = false;

  /* damage owed to the client from frames that were acquired (their dirty
   * rects consumed from IddCx) but then DROPPED before delivery (LGMP queue
   * full, Map/staging failure). IddCx considers that damage handled, so
   * unless it is re-delivered the client keeps the stale pixels forever —
   * drops cluster at the highest-damage moments (observed live 2026-07-06:
   * 'LGMP frame queue full' exactly at the login→desktop transition, giant
   * permanent ghosts). Folded into the next delivered frame's damage set;
   * overflow escalates to full-frame damage. */
  RECT     m_pendingDamage[LG_MAX_DIRTY_RECTS] = {};
  unsigned m_nbPendingDamage = 0;
  bool     m_pendingFullDamage = false;

  /* record the (already debt-merged) damage set of a dropped frame */
  void AccumulatePendingDamage(const RECT * rects, unsigned count);

  LONG     m_copyBusyDrops = 0;
  LONG     m_systemFrameCount = 0;
  LONG     m_systemAcquireFailures = 0;

  /* set when the processor terminates ITSELF (cursor thread topology loss)
   * rather than being terminated by the destructor — such exits must still
   * escalate to the monitor lifecycle like any other self-exit */
  volatile LONG m_selfTerminated = 0;

  /* holding the device context's processor-serial semaphore (acquired on the
   * processing thread before bring-up, released at the end of teardown after
   * the D3D devices are gone) — see CIndirectDeviceContext::ProcessorSerial */
  bool m_serialHeld = false;

  static DWORD CALLBACK _SwapChainThread(LPVOID arg);
  void SwapChainThread();
  /* returns true if the loop ended because termination was requested
   * (destructor / cursor-thread terminate), false on a self-exit (error) —
   * the caller escalates self-exits to a monitor replug */
  bool SwapChainThreadCore();
  /* device bring-up, pool init and LGMP setup — runs on the processing
   * thread, NEVER in EvtIddCxMonitorAssignSwapChain: D3D11CreateDevice and
   * IddCxSwapChainSetDevice were both observed blocking for many seconds
   * during boot/restart pairing churn, and UMDF terminates the host when the
   * callback overruns (the FAILED_POST_START kill class, 2026-07-03) */
  bool InitDevices();
  HRESULT SetSwapChainDevice();

  static DWORD CALLBACK _CursorThread(LPVOID arg);
  bool QueryHWCursor();
  void CursorThread();

  static void CompletionFunction(
    CD3D12CommandQueue * queue, bool result, void * param1, void * param2);
  HRESULT FinishProcessingFrame(const char * reason);
  bool SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer, unsigned dirtyRectCount);
  bool SwapChainNewFrameSystem(const IDDCX_SYSTEM_BUFFER_INFO& sysBuffer, unsigned dirtyRectCount);
  /* the D3D11 readback path (the ACTIVE path on Helios — D3D12 device creation
   * is unsupported there). The caller owns FinishProcessingFrame and calls it
   * AFTER this returns: the acquired buffer must stay unreleased until the
   * staging Map has completed, or dwm re-renders into it before the GPU copy
   * executes and the delivered content is newer than the dirty rects describe
   * (the persistent-ghost mechanism, 2026-07-06) */
  bool SwapChainNewFrameD3D11(ComPtr<IDXGIResource> acquiredBuffer,
    unsigned dirtyRectCount, unsigned moveRegionCount);

  /* D3D11 readback path: staging texture reused across frames (recreated on
   * format/size change only — a per-frame CreateTexture2D is a full venus
   * round-trip on Helios) */
  ComPtr<ID3D11Texture2D> m_d11Staging;
  D3D11_TEXTURE2D_DESC    m_d11StagingDesc = {};

  /* D3D11 readback path per-stage QPC telemetry, aggregated every 300 frames
   * (rects fetch / staging / copy submit / map sync / memcpy / lgmp publish +
   * inter-frame gap) — the swapchain thread is fully serialized, so these six
   * stages ARE the delivered frame rate */
  struct
  {
    LONGLONG rects, staging, copy, map, memcpyKvmfr, lgmp, gap;
    LONGLONG mapMax, gapMax;
    LONGLONG lastFrameEnd, windowStart;
    unsigned frames, stagingRecreates;
  } m_d11Stats = {};

public:
  CSwapChainProcessor(IDDCX_MONITOR monitor, CIndirectDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    LUID renderAdapter, HANDLE newFrameEvent);
  ~CSwapChainProcessor();

  /* the object MUST be owned by a std::shared_ptr before Start() is called
   * (the thread acquires its reference via shared_from_this) */
  HRESULT Start();

  /* signal-only termination; safe to call from any IddCx DDI callback */
  void Stop();

  /* Call before destroying the processor when AssignSwapChain returns a
   * failure status: IddCx retains ownership of the swapchain on failure and
   * deletes it itself. Only valid while the processing thread has not
   * started (the thread deletes the handle on exit). */
  void DisownSwapChain() { m_hSwapChain = nullptr; }

  CIndirectDeviceContext * GetDevice() { return m_devContext; }
  std::shared_ptr<CD3D12Device> GetD3D12Device() { return m_dx12Device; }
};
