#ifndef ALPAKADEVICEACC_H
#define ALPAKADEVICEACC_H

#include <vector>

#include <alpaka/alpaka.hpp>

#include "AlpakaCore/alpakaConfig.h"

namespace cms::alpakatools {

  // alpaka host device
  inline const alpaka_common::DevHost host = alpaka::getDevByIdx<alpaka_common::PltfHost>(0u);

  // alpaka accelerator devices
  template <typename TPlatform>
  inline std::vector<alpaka::Dev<TPlatform>> devices;

  template <typename TPlatform>
  std::vector<alpaka::Dev<TPlatform>> enumerate() {
    using Device = alpaka::Dev<TPlatform>;
    using Platform = TPlatform;

    std::vector<Device> devices;
    uint32_t n = alpaka::getDevCount<Platform>();
    devices.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      devices.push_back(alpaka::getDevByIdx<Platform>(i));
    }
    return devices;
  }

}  // namespace cms::alpakatools

#endif  // ALPAKADEVICEACC_H
