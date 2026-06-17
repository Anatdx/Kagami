#pragma once

#include <cstdint>

#ifndef __u32
using __u32 = std::uint32_t;
#endif

#ifndef __s32
using __s32 = std::int32_t;
#endif

#ifndef __aligned_u64
using __aligned_u64 = std::uint64_t __attribute__((aligned(8)));
#endif

#include "kagami/kasumi_uapi.h"
