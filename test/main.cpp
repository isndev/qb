#include "assert.h"
#include "ActorMock.h"

template <template <typename T>typename _ActorTest, typename _SharedData = void>
struct TEST {
	static void pingpong(std::string const &name)
	{
		for (int i = 0; i < 100; ++i) {
			test("PingPong Core0/1 (" + name + ")", []() {
				cube::Main < LinkedCoreData <_SharedData, PhysicalCore < 0 >, PhysicalCore < 1 >> > main;
				for (int i = 0; i < 100; ++i) {
					main.template addActor<1, _ActorTest>(main.template addActor<0, _ActorTest>());
				}
				main.start();
				main.join();
				return 0;
			});
		}
		for (int i = 0; i < 100; ++i) {
			test("PingPong Core0/1 & Core2/3 (" + name + ")", []() {
				cube::Main < LinkedCoreData <_SharedData, PhysicalCore < 0 >, PhysicalCore < 1 >>,
					LinkedCoreData <_SharedData, PhysicalCore < 2 >, PhysicalCore < 3 >> > main;
				for (int i = 0; i < 100; ++i) {
					main.template addActor<1, _ActorTest>(main.template addActor<0, _ActorTest>());
					main.template addActor<3, _ActorTest>(main.template addActor<2, _ActorTest>());
				}
				main.start();
				main.join();
				return 0;
			});
		}
	}
};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./", "cube.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN);

	TEST<ActorMock_Tiny>::pingpong("TinyEvent");
	TEST<ActorMock_Big>::pingpong("BigEvent");
	TEST<ActorMock_Dynamic>::pingpong("DynamicEvent");

    system("Pause");
    return 0;
}