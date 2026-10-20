#pragma once

#include <string>

namespace sdcard {

bool mount(std::string& error);
bool isMounted();

}  // namespace sdcard
