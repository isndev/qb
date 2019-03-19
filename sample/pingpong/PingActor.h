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

// PingActor.h file
#ifndef PINGACTOR_H_
#define PINGACTOR_H_
#include <qb/actor.h>
#include "PingPongEvent.h"
#include "latency.hpp"

class PingActor
        : public qb::Actor 
{
    const qb::ActorId _id_pong; 
    pg::latency<1000 * 1000, 900000> latency;
	
public:
    PingActor() = delete; 

    explicit PingActor(const qb::ActorId id_pong)
            : _id_pong(id_pong) {}
			
	bool onInit() override final {
        registerEvent<PingPongEvent>(*this);
        auto &event = push<PingPongEvent>(_id_pong); 
		event.counter = 0;                    
		event.timestamp = std::chrono::high_resolution_clock::now();     

        return true;
    }
	
    void on(PingPongEvent &event) 
	{
		latency.add(std::chrono::high_resolution_clock::now() - event.timestamp);

		if (event.counter > 0 && event.counter % 1000000 == 0)
		{
			latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
			exit(0);
		}
		
		auto &e = push<PingPongEvent>(_id_pong);
		e.counter = event.counter+1;                    
		e.timestamp = std::chrono::high_resolution_clock::now(); 
    }
};

#endif