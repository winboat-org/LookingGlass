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

#include "CD3D11Device.h"
#include "CDebug.h"

HRESULT CD3D11Device::Init()
{
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
  if (FAILED(hr))
    return hr;

  static const D3D_FEATURE_LEVEL featureLevels[] =
  {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0
  };

  hr = m_factory->EnumAdapterByLuid(m_adapterLuid, IID_PPV_ARGS(&m_adapter));
  if (FAILED(hr))
    return hr;

  DXGI_ADAPTER_DESC1 desc = {};
  hr = m_adapter->GetDesc1(&desc);
  if (FAILED(hr))
    return hr;

  m_isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

  DEBUG_INFO("D3D11CreateDevice: begin flags=BGRA featureLevels=11_1,11_0,10_1,10_0");
  hr = D3D11CreateDevice(
    m_adapter.Get(),
    D3D_DRIVER_TYPE_UNKNOWN,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    featureLevels,
    ARRAYSIZE(featureLevels),
    D3D11_SDK_VERSION,
    &m_device,
    &m_featureLevel,
    &m_context);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "D3D11CreateDevice failed");
    return hr;
  }

  DEBUG_INFO("Feature Level: 0x%x", m_featureLevel);

  hr = m_device.As(&m_device5);
  if (FAILED(hr))
    DEBUG_INFO("ID3D11Device5 unavailable; using base ID3D11Device path (0x%08x)", hr);

  hr = m_context.As(&m_context4);
  if (FAILED(hr))
    DEBUG_INFO("ID3D11DeviceContext4 unavailable; using base ID3D11DeviceContext path (0x%08x)", hr);

  return S_OK;
}
