#include <gtest/gtest.h>
#include <cube/actor.h>

TEST(Example, Equals) {
  cube::Main main({0, 1});
  EXPECT_EQ(1, 1);
}