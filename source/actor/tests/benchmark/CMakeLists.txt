
#cxx_benchmark(${PROJECT_NAME}-benchmark-example  "${PROJECT_NAME}" benchmark_example.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-pingpong  "${PROJECT_NAME}" ping-pong/main.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-chain-event  "${PROJECT_NAME}" chain-event/main.cpp)