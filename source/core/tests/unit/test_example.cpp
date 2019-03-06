#include <gtest/gtest.h>
#include <cube/actor.h>

TEST(Example, StartEngine) {
  qb::Main main({0});

  main.start();
  main.join();
  EXPECT_TRUE(main.hasError());
}