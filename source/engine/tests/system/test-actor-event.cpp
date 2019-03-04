#include <gtest/gtest.h>
#include <cube/actor.h>
#include <cube/main.h>
#include <random>
#include <numeric>

class TestEvent : public cube::Event
{
    uint8_t  _data[32];
    uint32_t _sum;
public:
    TestEvent() : _sum(0) {
        std::random_device                  rand_dev;
        std::mt19937                        generator(rand_dev());

        std::uniform_int_distribution<int> random_number(0, 255);
        std::generate(std::begin(_data), std::end(_data), [&](){
            auto number = static_cast<uint8_t>(random_number(generator));
            _sum += number;
            return number;
        });
    }

    bool checkSum() const {
        return std::accumulate(std::begin(_data), std::end(_data), 0u) == _sum;
    }
};

class TestActorReceiver
        : public cube::Actor
{
    const uint32_t _max_events;
    uint32_t       _count;
public:
    TestActorReceiver(uint32_t const max_events)
        : _max_events(max_events), _count(0) {}

    virtual bool onInit() override final {
      registerEvent<TestEvent>(*this);
      return true;
    }

    void on(TestEvent const &event) {
        EXPECT_TRUE(event.checkSum());
        if (++_count >= _max_events)
            kill();
    }
};

class TestActorSender
        : public cube::Actor
        , public cube::ICallback
{
    const uint32_t _max_events;
    const cube::ActorId _to;
    uint32_t       _count;
public:
    TestActorSender(uint32_t const max_events, cube::ActorId const to)
            : _max_events(max_events), _to(to), _count(0) {}

    virtual bool onInit() override final {
        registerCallback(*this);
        return true;
    }

    virtual void onCallback() override final {
        push<TestEvent>(_to);
        if (++_count >= _max_events)
            kill();
    }
};

TEST(ActorEvent, PushMonoCore) {
    cube::Main main({0});
    const auto max_events = 1024u;
    for (auto i = 0u; i < 1024u; ++i) {
        main.addActor<TestActorSender>(0, max_events, main.addActor<TestActorReceiver>(0, max_events));
    }

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorEvent, PushMultiCore) {
    const auto max_core = std::thread::hardware_concurrency();
    const auto max_events = 1024u;

    EXPECT_GT(max_core, 1u);
    cube::Main main(cube::CoreSet::build(max_core));

    for (auto i = 0u; i < max_core; ++i)
    {
        for (auto j = 0u; j < 1024u; ++j) {
            main.addActor<TestActorSender>(i, max_events, main.addActor<TestActorReceiver>(((i + 1) % max_core), max_events));
        }
    }

    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
}