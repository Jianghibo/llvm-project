; RUN: opt -passes='dse,print<memoryssa>' -verify-memoryssa \
; RUN:   -disable-output %s 2>&1 | FileCheck %s --check-prefix=MSSA
; RUN: opt -passes='dse,early-cse<memssa>' -verify-memoryssa -verify-each \
; RUN:   -S %s | FileCheck %s

; Removing the redundant store in right must also simplify the MemoryPhi in
; join, including the final store's cached clobber. Otherwise EarlyCSE cannot
; prove that the store writes back the value loaded in entry.
; https://github.com/llvm/llvm-project/issues/221594
define void @redundant_store(i1 %c, ptr %p, ptr %q, i1 %v) {
; MSSA-LABEL: MemorySSA for function: redundant_store
; MSSA-NOT: MemoryPhi
; MSSA: ret void
; CHECK-LABEL: define void @redundant_store(
; CHECK: store i1 %v, ptr %q
; CHECK: right:
; CHECK-NEXT: br label %join
; CHECK: join:
; CHECK-NEXT: ret void
entry:
  store i1 %v, ptr %q
  %x = load i32, ptr %p
  br i1 %c, label %left, label %right

left:
  br label %join

right:
  store i1 %v, ptr %q
  br label %join

join:
  store i32 %x, ptr %p
  ret void
}

; Only join1 is a direct phi user of the deleted store. Simplifying it must
; recursively simplify join2 as well.
define void @cascading_phis(i1 %c1, i1 %c2, ptr %p, ptr %q, i1 %v) {
; MSSA-LABEL: MemorySSA for function: cascading_phis
; MSSA-NOT: MemoryPhi
; MSSA: ret void
; CHECK-LABEL: define void @cascading_phis(
; CHECK: store i1 %v, ptr %q
; CHECK: right:
; CHECK-NEXT: br label %join1
; CHECK: join2:
; CHECK-NEXT: ret void
entry:
  store i1 %v, ptr %q
  %x = load i32, ptr %p
  br i1 %c1, label %split, label %join2

split:
  br i1 %c2, label %left, label %right

left:
  br label %join1

right:
  store i1 %v, ptr %q
  br label %join1

join1:
  br label %join2

join2:
  store i32 %x, ptr %p
  ret void
}

; Both phis are direct users of the deleted store. Cleaning up one candidate
; can recursively delete another candidate in the batch.
define void @multiple_candidates(i1 %c1, i1 %c2, ptr %p, ptr %q, i1 %v) {
; MSSA-LABEL: MemorySSA for function: multiple_candidates
; MSSA-NOT: MemoryPhi
; MSSA: ret void
; CHECK-LABEL: define void @multiple_candidates(
; CHECK: store i1 %v, ptr %q
; CHECK: right:
; CHECK-NEXT: br i1 %c2, label %join1, label %join2
; CHECK: join2:
; CHECK-NEXT: ret void
entry:
  store i1 %v, ptr %q
  %x = load i32, ptr %p
  br i1 %c1, label %left, label %right

left:
  br label %join1

right:
  store i1 %v, ptr %q
  br i1 %c2, label %join1, label %join2

join1:
  br label %join2

join2:
  store i32 %x, ptr %p
  ret void
}

; The exit store kills the loop store, leaving a phi with an entry definition
; and a self-reference. The two exit stores must remain because p may alias q.
define void @loop_phi(i1 %c, ptr %p, ptr %q, i1 %v, i1 %w) {
; MSSA-LABEL: MemorySSA for function: loop_phi
; MSSA-NOT: MemoryPhi
; MSSA: ret void
; CHECK-LABEL: define void @loop_phi(
; CHECK: store i1 %v, ptr %q
; CHECK: loop:
; CHECK-NEXT: br i1 %c, label %loop, label %exit
; CHECK: exit:
; CHECK-NEXT: store i1 %w, ptr %q
; CHECK-NEXT: store i32 %x, ptr %p
; CHECK-NEXT: ret void
entry:
  store i1 %v, ptr %q
  %x = load i32, ptr %p
  br label %loop

loop:
  store i1 %v, ptr %q
  br i1 %c, label %loop, label %exit

exit:
  store i1 %w, ptr %q
  store i32 %x, ptr %p
  ret void
}

; Deleting a store does not necessarily make its phi users trivial. The right
; store is redundant, but the left store may clobber p and must remain.
define void @nontrivial_phi(i1 %c, ptr %p, ptr %q, i1 %v, i1 %w) {
; MSSA-LABEL: MemorySSA for function: nontrivial_phi
; MSSA: MemoryPhi
; MSSA: ret void
; CHECK-LABEL: define void @nontrivial_phi(
; CHECK: store i1 %v, ptr %q
; CHECK: left:
; CHECK-NEXT: store i1 %w, ptr %q
; CHECK-NEXT: br label %join
; CHECK: right:
; CHECK-NEXT: br label %join
; CHECK: join:
; CHECK-NEXT: store i32 %x, ptr %p
; CHECK-NEXT: ret void
entry:
  store i1 %v, ptr %q
  %x = load i32, ptr %p
  br i1 %c, label %left, label %right

left:
  store i1 %w, ptr %q
  br label %join

right:
  store i1 %v, ptr %q
  br label %join

join:
  store i32 %x, ptr %p
  ret void
}
