
#cxx_benchmark(${PROJECT_NAME}-benchmark-example  "${PROJECT_NAME}" benchmark_example.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-pingpong  "${PROJECT_NAME}" bm-ping-pong.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-chain-event  "${PROJECT_NAME}" bm-chain-event.cpp)