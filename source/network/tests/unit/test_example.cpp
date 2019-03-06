#include <gtest/gtest.h>
#include <cube/network/ip.h>

TEST(Example, Equals) {
  qb::network::ip ip("192.168.0.123");
  EXPECT_EQ(ip.toString(), "192.168.0.123");
}