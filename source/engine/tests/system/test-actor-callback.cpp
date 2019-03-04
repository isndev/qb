#include <gtest/gtest.h>
#include <cube/actor.h>
#include <cube/main.h>

struct UnregisterCallbackEvent : public cube::Event {};

class TestActor
        : public cube::Actor
        , public cube::ICallback
{
    const uint64_t _max_loop;
    uint64_t _count_loop;
public:
    TestActor() = delete;
    explicit TestActor(uint64_t const max_loop)
      : _max_loop(max_loop), _count_loop(0) {}

    ~TestActor() {
        if (_max_loop == 1000) {
            EXPECT_EQ(_count_loop, _max_loop);
        }
    }

    virtual bool onInit() override final {
        registerEvent<UnregisterCallbackEvent>(*this);
        if (_max_loop)
            registerCallback(*this);
        else
            kill();
        return true;
    }

    virtual void onCallback() override final {
        if (_max_loop == 10000)
            push<UnregisterCallbackEvent>(id());
        if (++_count_loop >= _max_loop)
            kill();
    }

    void on(UnregisterCallbackEvent &) {
        unregisterCallback(*this);
        push<cube::KillEvent>(id());
    }
};

TEST(CallbackActor, ShouldNotCallOnCallbackIfNotRegistred) {
    cube::Main main({0});

    main.addActor<TestActor>(0, 0);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(CallbackActor, ShouldCallOnCallbackIfRegistred) {
    cube::Main main({0});

    main.addActor<TestActor>(0, 1000);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}

TEST(CallbackActor, ShouldNotCallOnCallbackAnymoreIfUnregistred) {
    cube::Main main({0});

    main.addActor<TestActor>(0, 10000);

    main.start(false);
    EXPECT_FALSE(main.hasError());
}