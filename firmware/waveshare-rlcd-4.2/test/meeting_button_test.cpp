// SPDX-License-Identifier: Apache-2.0
#include "charadock/input.hpp"
#include <cassert>
using namespace charadock::rlcd;
int main() {
  DebouncedButton button(ButtonId::Boot, 3000);
  assert(button.update(true, 0).empty());
  assert(button.update(true, 26).front().action == ButtonAction::Pressed);
  assert(button.update(false, 100).empty());
  const auto shortPress = button.update(false, 126);
  assert(shortPress.size() == 1);
  assert(shortPress.front().action == ButtonAction::ShortPress);
  assert(button.update(false, 200).empty());
  button.update(true, 300);
  button.update(true, 326);
  const auto diagnostic = button.update(true, 3326);
  assert(diagnostic.size() == 1);
  assert(diagnostic.front().action == ButtonAction::LongPress);
  assert(button.update(true, 3400).empty());
  button.update(false, 3500);
  const auto released = button.update(false, 3526);
  assert(released.size() == 1);
  assert(released.front().action == ButtonAction::LongRelease);
}
