#include "assert.h"
#include "ActorMock.h"

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./", "cube.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN);
    for (int i = 0; i < 100; ++i) {
        test("PingPong Core0-1", []() {
			cube::Main < LinkedCore < PhysicalCore < 0 >, PhysicalCore < 1 >> > main;
            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, ActorMock>(main.template addActor<0, ActorMock>());
            }
            main.start();
            main.join();
            return 0;
        });
    }
    for (int i = 0; i < 100; ++i) {
        test("PingPong Core0-1| Core2-3", []() {
            cube::Main < LinkedCore < PhysicalCore < 0 > , PhysicalCore < 1 >>,
                    LinkedCore < PhysicalCore < 2 >, PhysicalCore < 3 >> > main;
            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, ActorMock>(main.template addActor<0, ActorMock>());
                main.template addActor<3, ActorMock>(main.template addActor<2, ActorMock>());
            }
            main.start();
            main.join();
            return 0;
        });
    }
    system("Pause");
    return 0;
}