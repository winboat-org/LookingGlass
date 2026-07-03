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

#include "CIndirectMonitorContext.h"
#include "CPlatformInfo.h"
#include "CDebug.h"
#include "CPipeServer.h"

CIndirectMonitorContext::CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device) :
  m_monitor(monitor),
  m_devContext(device)
{
}

CIndirectMonitorContext::~CIndirectMonitorContext()
{
  UnassignSwapChain();
}

NTSTATUS CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
  UnassignSwapChain();

  DEBUG_INFO("AssignSwapChain: renderAdapter LUID=%08x:%08x (device bring-up deferred to the processor thread)",
    (unsigned)renderAdapter.HighPart, (unsigned)renderAdapter.LowPart);

  /* NO blocking work in this callback. D3D11CreateDevice and
   * IddCxSwapChainSetDevice were both observed blocking for 6-22+ seconds
   * during boot/restart pairing churn, and UMDF terminates the host when
   * EvtIddCxMonitorAssignSwapChain overruns — every FAILED_POST_START kill
   * of 2026-07-03 (boots #1/#2/#4 and the warm deploy crashes) happened with
   * this callback still executing. All bring-up (D3D11/D3D12 devices, pools,
   * LGMP, SetDevice) now runs on the processor thread
   * (CSwapChainProcessor::InitDevices); a bring-up failure self-exits the
   * thread, which queues a monitor replug — the single recovery primitive
   * (this also covers the born-abandoned first swapchain of a boot, which
   * previously returned ABANDON_SWAPCHAIN from here). */
  m_swapChain = std::make_shared<CSwapChainProcessor>(m_monitor, m_devContext, swapChain, renderAdapter, newFrameEvent);
  HRESULT startHr = m_swapChain->Start();
  if (FAILED(startHr))
  {
    DEBUG_ERROR_HR(startHr, "AssignSwapChain: failed to start swapchain processor");
    /* returning failure means IddCx keeps ownership of the swapchain, so the
     * processor must not delete it in its destructor */
    m_swapChain->DisownSwapChain();
    m_swapChain.reset();
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }
  return STATUS_SUCCESS;
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  /* signal + release, NEVER join: this runs inside
   * EvtIddCxMonitorUnassignSwapChain (or monitor cleanup), and IddCx's
   * watchdog terminates WUDFHost when a DDI callback overruns. The
   * processing threads hold their own references and finish teardown
   * (including the swapchain object delete) when their current blocking
   * call returns. */
  if (m_swapChain)
  {
    m_swapChain->Stop();
    m_swapChain.reset();
  }
}
