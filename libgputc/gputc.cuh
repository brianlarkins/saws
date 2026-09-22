#pragma once

// libgputc: GPU SAWS work stealing. Include this from the application's .cu.
//
//   struct MyTask {
//     using Body = MyNode;                    // trivially copyable
//     template<class Ctx>
//     static __device__ void execute(Ctx& ctx, const MyNode& node, bool valid);
//   };
//
//   gputc::Stats s = gputc::run<MyTask>(root);

#include "config.hpp"
#include "stats.hpp"
#include "context.cuh"
#include "kernel.cuh"
#include "run.cuh"
