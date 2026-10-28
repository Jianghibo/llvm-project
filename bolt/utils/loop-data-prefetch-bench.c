//===- loop-data-prefetch-bench.c - Prefetch experiment ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Build with -O2 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops
// -fno-omit-frame-pointer -pthread -Wl,--emit-relocs,--build-id. Compare BOLT
// with the same BOLT options plus --enable-loop-data-prefetch and a distance.
// This is a controlled experiment, not a production acceptance workload.
//===----------------------------------------------------------------------===//

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Record {
  uint64_t Key, Value;
  char Padding[112];
};

__attribute__((noinline)) uint64_t sequential(const uint64_t *Data, size_t N) {
  uint64_t Sum = 0;
  for (size_t I = 0; I < N; ++I)
    Sum += Data[I];
  return Sum;
}

__attribute__((noinline)) uint64_t strided(const uint64_t *Data, size_t N) {
  uint64_t Sum = 0;
  for (size_t I = 0; I < N; I += 64)
    Sum += Data[I];
  return Sum;
}

__attribute__((noinline)) uint64_t records(const uint64_t *Data, size_t N) {
  const struct Record *R = (const struct Record *)Data;
  uint64_t Sum = 0;
  for (size_t I = 0; I < N / 16; ++I)
    Sum += R[I].Key + R[I].Value;
  return Sum;
}

__attribute__((noinline)) uint64_t streams2(const uint64_t *Data, size_t N) {
  const size_t Half = N / 2;
  uint64_t Sum = 0;
  for (size_t I = 0; I < Half; ++I)
    Sum += Data[I] + Data[Half + I];
  return Sum;
}

__attribute__((noinline)) uint64_t streams4(const uint64_t *Data, size_t N) {
  const size_t Quarter = N / 4;
  uint64_t Sum = 0;
  for (size_t I = 0; I < Quarter; ++I)
    Sum += Data[I] + Data[Quarter + I] + Data[Quarter * 2 + I] +
           Data[Quarter * 3 + I];
  return Sum;
}

struct Work {
  uint64_t (*Run)(const uint64_t *, size_t);
  const uint64_t *Data;
  size_t N;
  unsigned Repetitions;
  uint64_t Sum, Expected;
  pthread_barrier_t *Barrier;
};

static void *worker(void *Arg) {
  struct Work *W = Arg;
  W->Expected = W->Run(W->Data, W->N) * W->Repetitions;
  if (W->Barrier) {
    pthread_barrier_wait(W->Barrier); // Warmup complete.
    pthread_barrier_wait(W->Barrier); // Timed start.
  }
  uint64_t Sum = 0;
  for (unsigned R = 0; R < W->Repetitions; ++R)
    Sum += W->Run(W->Data, W->N);
  W->Sum = Sum;
  return NULL;
}

static double now(void) {
  struct timespec TS;
  clock_gettime(CLOCK_MONOTONIC, &TS);
  return TS.tv_sec + TS.tv_nsec * 1e-9;
}

int main(int Argc, char **Argv) {
  if (Argc != 4 && Argc != 5) {
    fprintf(stderr,
            "usage: %s sequential|strided|records|streams2|streams4 MiB "
            "repetitions [threads]\n",
            Argv[0]);
    return 1;
  }
  uint64_t (*Run)(const uint64_t *, size_t) = NULL;
  if (!strcmp(Argv[1], "sequential"))
    Run = sequential;
  else if (!strcmp(Argv[1], "strided"))
    Run = strided;
  else if (!strcmp(Argv[1], "records"))
    Run = records;
  else if (!strcmp(Argv[1], "streams2"))
    Run = streams2;
  else if (!strcmp(Argv[1], "streams4"))
    Run = streams4;
  const unsigned Threads = Argc == 5 ? strtoul(Argv[4], NULL, 10) : 1;
  const size_t MiB = strtoull(Argv[2], NULL, 10);
  const unsigned Repetitions = strtoul(Argv[3], NULL, 10);
  if (!Run || !MiB || MiB > 4096 || !Repetitions || !Threads || Threads > 64)
    return 1;
  const size_t N = MiB * 1024 * 1024 / sizeof(uint64_t);
  uint64_t *Data = malloc(N * sizeof(uint64_t));
  if (!Data)
    return 1;
  for (size_t I = 0; I < N; ++I)
    Data[I] = I % 997;
  struct Work Work[64];
  pthread_t IDs[64];
  pthread_barrier_t Barrier;
  if (Threads > 1 && pthread_barrier_init(&Barrier, NULL, Threads + 1))
    return 1;
  for (unsigned T = 0; T < Threads; ++T) {
    Work[T] = (struct Work){
        Run, Data + T * (N / Threads),     N / Threads, Repetitions, 0,
        0,   Threads > 1 ? &Barrier : NULL};
    if (Threads > 1 && pthread_create(&IDs[T], NULL, worker, &Work[T]))
      return 1;
  }
  if (Threads > 1)
    pthread_barrier_wait(&Barrier);
  // Keep single-thread warmup outside the measured interval too.
  if (Threads == 1)
    Work[0].Expected = Run(Data, N) * Repetitions;
  const double Start = now();
  if (Threads > 1) {
    pthread_barrier_wait(&Barrier);
    for (unsigned T = 0; T < Threads; ++T)
      pthread_join(IDs[T], NULL);
    pthread_barrier_destroy(&Barrier);
  } else {
    for (unsigned R = 0; R < Repetitions; ++R)
      Work[0].Sum += Run(Data, N);
  }
  const double Seconds = now() - Start;
  uint64_t Sum = 0;
  int Failed = 0;
  for (unsigned T = 0; T < Threads; ++T) {
    Sum += Work[T].Sum;
    Failed |= Work[T].Sum != Work[T].Expected;
  }
  printf("%s %zu %u %.9f %" PRIu64 " %u\n", Argv[1], MiB, Repetitions, Seconds,
         Sum, Threads);
  free(Data);
  return Failed;
}
