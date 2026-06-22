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

void CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
reInit:
  UnassignSwapChain();

  DEBUG_INFO("AssignSwapChain: renderAdapter LUID=%08x:%08x",
    (unsigned)renderAdapter.HighPart, (unsigned)renderAdapter.LowPart);

  m_dx11Device = std::make_shared<CD3D11Device>(renderAdapter);
  HRESULT dx11hr = m_dx11Device->Init();
  if (FAILED(dx11hr))
  {
    DEBUG_ERROR_HR(dx11hr, "AssignSwapChain: CD3D11Device::Init FAILED (this gates the whole capture path)");
    WdfObjectDelete(swapChain);
    return;
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
      DEBUG_ERROR("AssignSwapChain: CD3D12Device::Init FAILURE (D3D12 unavailable on render adapter)");
      WdfObjectDelete(swapChain);
      return;

    case CD3D12Device::RETRY:
      DEBUG_INFO("AssignSwapChain: CD3D12Device::Init RETRY");
      m_dx12Device.reset();
      m_dx11Device.reset();
      goto reInit;
  }
  
  if (!m_devContext->SetupLGMP(alignSize))
  {
    WdfObjectDelete(swapChain);
    DEBUG_ERROR("SetupLGMP failed");
    return;
  }

  m_swapChain.reset(new CSwapChainProcessor(m_monitor, m_devContext, swapChain, m_dx11Device, m_dx12Device, newFrameEvent));
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  m_swapChain.reset();  
  m_dx11Device.reset();
  m_dx12Device.reset();
}
