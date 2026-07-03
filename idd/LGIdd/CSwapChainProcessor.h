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

class CSwapChainProcessor
{
private:
  IDDCX_MONITOR                   m_monitor;
  CIndirectDeviceContext        * m_devContext;
  IDDCX_SWAPCHAIN                 m_hSwapChain;
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
  LONG     m_copyBusyDrops = 0;
  LONG     m_systemFrameCount = 0;
  LONG     m_systemAcquireFailures = 0;

  /* set when the processor terminates ITSELF (cursor thread topology loss)
   * rather than being terminated by the destructor — such exits must still
   * escalate to the monitor lifecycle like any other self-exit */
  volatile LONG m_selfTerminated = 0;

  static DWORD CALLBACK _SwapChainThread(LPVOID arg);
  void SwapChainThread();
  /* returns true if the loop ended because termination was requested
   * (destructor / cursor-thread terminate), false on a self-exit (error) —
   * the caller escalates self-exits to a monitor replug */
  bool SwapChainThreadCore();
  HRESULT SetSwapChainDevice();

  static DWORD CALLBACK _CursorThread(LPVOID arg);
  bool QueryHWCursor();
  void CursorThread();

  static void CompletionFunction(
    CD3D12CommandQueue * queue, bool result, void * param1, void * param2);
  HRESULT FinishProcessingFrame(const char * reason);
  bool SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer, unsigned dirtyRectCount);
  bool SwapChainNewFrameSystem(const IDDCX_SYSTEM_BUFFER_INFO& sysBuffer, unsigned dirtyRectCount);
  bool SwapChainNewFrameD3D11(ComPtr<IDXGIResource> acquiredBuffer, unsigned dirtyRectCount, bool frameAlreadyReleased);

public:
  CSwapChainProcessor(IDDCX_MONITOR monitor, CIndirectDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, HANDLE newFrameEvent);
  ~CSwapChainProcessor();
  HRESULT Start();

  /* Call before destroying the processor when AssignSwapChain returns a
   * failure status: IddCx retains ownership of the swapchain on failure and
   * deletes it itself. Only valid while the processing thread has not
   * started (the thread deletes the handle on exit). */
  void DisownSwapChain() { m_hSwapChain = nullptr; }

  CIndirectDeviceContext * GetDevice() { return m_devContext; }
  std::shared_ptr<CD3D12Device> GetD3D12Device() { return m_dx12Device; }
};
