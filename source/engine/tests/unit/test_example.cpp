#include <gtest/gtest.h>
#include <cube/actor.h>

TEST(Example, StartEngine) {
  cube::Main main({0});

  main.start();
  main.join();
  EXPECT_TRUE(main.hasError());
}