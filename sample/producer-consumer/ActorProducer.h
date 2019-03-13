/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#include <vector>
#include <random>
#include <qb/actor.h>
#include "MyEvent.h"

#ifndef ACTORPRODUCER_H_
# define ACTORPRODUCER_H_

class ActorProducer
        : public qb::Actor     // /!\ should inherit from qb actor
        , public qb::ICallback // (optional) required to register actor callback
{
    std::vector<qb::ActorId> const &_consumerIds;
    std::random_device          _rand_dev;
    std::mt19937                _generator;
    std::uniform_int_distribution<int> _random_number;
public:
    ActorProducer() = delete;  // delete default constructor
    ActorProducer(std::vector<qb::ActorId> const &ids) // constructor with parameters
        : _consumerIds(ids)
        , _rand_dev()
        , _generator(_rand_dev())
        , _random_number(0, static_cast<int>(ids.size() - 1))
    {}

    ~ActorProducer() = default;

    // will call this function before adding Actor
    virtual bool onInit() override final {
        registerCallback(*this);// each core loop will call onCallback
        return true;            // init ok, MyActor will be added
    }

    // will call this function each core loop
    virtual void onCallback() override final {
        to(_consumerIds[_random_number(_generator)]).push<MyEvent>();
    }
};

#endif