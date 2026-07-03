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
  /* cap the RETRY re-init loop: unbounded, it can spin forever INSIDE the
   * AssignSwapChain callback, wedging IddCx's calling thread */
  unsigned reInitAttempts = 0;
reInit:
  UnassignSwapChain();

  DEBUG_INFO("AssignSwapChain: renderAdapter LUID=%08x:%08x",
    (unsigned)renderAdapter.HighPart, (unsigned)renderAdapter.LowPart);

  m_dx11Device = std::make_shared<CD3D11Device>(renderAdapter);
  HRESULT dx11hr = m_dx11Device->Init();
  if (FAILED(dx11hr))
  {
    DEBUG_ERROR_HR(dx11hr, "AssignSwapChain: CD3D11Device::Init FAILED (this gates the whole capture path)");
    /* Do NOT WdfObjectDelete here: ownership of hSwapChain only transfers to
     * the driver when EvtIddCxMonitorAssignSwapChain returns success. On a
     * failure return IddCx deletes the swapchain itself
     * (IddMonitor::HandleNewSwapChain), so deleting it here double-deletes
     * the FxObject and crashes WUDFHost -> the IDD goes Code 43. */
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }
  DEBUG_INFO("AssignSwapChain: CD3D11Device::Init OK");

  UINT64 alignSize = CPlatformInfo::GetPageSize();
  m_dx12Device = std::make_shared<CD3D12Device>(renderAdapter);
  switch (m_dx12Device->Init(m_devContext->GetIVSHMEM(), alignSize))
  {
    case CD3D12Device::SUCCESS:
      DEBUG_INFO("AssignSwapChain: CD3D12Device::Init OK");
      break;

    case CD3D12Device::FAILURE:
      DEBUG_WARN("AssignSwapChain: CD3D12Device::Init FAILURE; using D3D11 readback path");
      m_dx12Device.reset();
      alignSize = CPlatformInfo::GetPageSize();
      break;

    case CD3D12Device::RETRY:
      DEBUG_INFO("AssignSwapChain: CD3D12Device::Init RETRY");
      m_dx12Device.reset();
      m_dx11Device.reset();
      if (++reInitAttempts >= 3)
      {
        DEBUG_ERROR("AssignSwapChain: CD3D12Device::Init RETRY %u times, abandoning swapchain "
                    "(the OS will offer a new one; the monitor watchdog replugs if it never does)",
          reInitAttempts);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
      }
      goto reInit;
  }
  
  if (!m_devContext->SetupLGMP(alignSize))
  {
    DEBUG_ERROR("SetupLGMP failed");
    /* failure return -> IddCx owns and deletes the swapchain; see above */
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }

  m_swapChain.reset(new CSwapChainProcessor(m_monitor, m_devContext, swapChain, m_dx11Device, m_dx12Device, newFrameEvent));
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
  m_swapChain.reset();  
  m_dx11Device.reset();
  m_dx12Device.reset();
}
