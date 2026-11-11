#pragma once

#include <cstdint>

class ViewerLink;

namespace linkprobe {
bool start(ViewerLink& link);
void onAck(uint32_t seq);
}
