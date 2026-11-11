#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

Result grcdSessionOpen(Service* srv);

Result grcdSessionBegin(Service* srv);

Result grcdSessionTransfer(Service* srv, GrcStream stream, void* buffer, size_t size, u32* num_frames,
                           u32* data_size, u64* start_timestamp);

#ifdef __cplusplus
}
#endif
