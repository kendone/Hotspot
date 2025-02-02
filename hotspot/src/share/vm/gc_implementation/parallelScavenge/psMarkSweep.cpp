/*
 * Copyright (c) 2001, 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc_implementation/parallelScavenge/generationSizer.hpp"
#include "gc_implementation/parallelScavenge/parallelScavengeHeap.hpp"
#include "gc_implementation/parallelScavenge/psAdaptiveSizePolicy.hpp"
#include "gc_implementation/parallelScavenge/psMarkSweep.hpp"
#include "gc_implementation/parallelScavenge/psMarkSweepDecorator.hpp"
#include "gc_implementation/parallelScavenge/psOldGen.hpp"
#include "gc_implementation/parallelScavenge/psPermGen.hpp"
#include "gc_implementation/parallelScavenge/psScavenge.hpp"
#include "gc_implementation/parallelScavenge/psYoungGen.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "gc_implementation/shared/spaceDecorator.hpp"
#include "gc_interface/gcCause.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/referencePolicy.hpp"
#include "memory/referenceProcessor.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/fprofiler.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/vmThread.hpp"
#include "services/management.hpp"
#include "services/memoryService.hpp"
#include "utilities/events.hpp"
#include "utilities/stack.inline.hpp"

elapsedTimer        PSMarkSweep::_accumulated_time;
unsigned int        PSMarkSweep::_total_invocations = 0;
jlong               PSMarkSweep::_time_of_last_gc   = 0;
CollectorCounters*  PSMarkSweep::_counters = NULL;

void PSMarkSweep::initialize() {
  MemRegion mr = Universe::heap()->reserved_region();
  _ref_processor = new ReferenceProcessor(mr);     // a vanilla ref proc
  _counters = new CollectorCounters("PSMarkSweep", 1);
}

// This method contains all heap specific policy for invoking mark sweep.
// PSMarkSweep::invoke_no_policy() will only attempt to mark-sweep-compact
// the heap. It will do nothing further. If we need to bail out for policy
// reasons, scavenge before full gc, or any other specialized behavior, it
// needs to be added here.
//
// Note that this method should only be called from the vm_thread while
// at a safepoint!
//
// Note that the all_soft_refs_clear flag in the collector policy
// may be true because this method can be called without intervening
// activity.  For example when the heap space is tight and full measure
// are being taken to free space.

/**
 * 执行Full Gc
 */
void PSMarkSweep::invoke(bool maximum_heap_compaction) {
  assert(SafepointSynchronize::is_at_safepoint(), "should be at safepoint");
  assert(Thread::current() == (Thread*)VMThread::vm_thread(), "should be in vm thread");
  assert(!Universe::heap()->is_gc_active(), "not reentrant");

  ParallelScavengeHeap* heap = (ParallelScavengeHeap*)Universe::heap();
  GCCause::Cause gc_cause = heap->gc_cause();	//触发本次Gc的原因
  PSAdaptiveSizePolicy* policy = heap->size_policy();
  IsGCActiveMark mark;

  if (ScavengeBeforeFullGC) {
    PSScavenge::invoke_no_policy();
  }

  //是否清理所有的软/弱引用对象
  const bool clear_all_soft_refs = heap->collector_policy()->should_clear_all_soft_refs();

  int count = (maximum_heap_compaction)?1:MarkSweepAlwaysCompactCount;
  IntFlagSetting flag_setting(MarkSweepAlwaysCompactCount, count);
  PSMarkSweep::invoke_no_policy(clear_all_soft_refs || maximum_heap_compaction);
}

// This method contains no policy. You should probably
// be calling invoke() instead.
bool PSMarkSweep::invoke_no_policy(bool clear_all_softrefs) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at a safepoint");
  assert(ref_processor() != NULL, "Sanity");

  if (GC_locker::check_active_before_gc()) {
    return false;
  }

  printf("%s[%d] [tid: %lu]: 开始执行FullGC...\n", __FILE__, __LINE__, pthread_self());

  ParallelScavengeHeap* heap = (ParallelScavengeHeap*)Universe::heap();
  GCCause::Cause gc_cause = heap->gc_cause();	//触发本次Gc的原因
  assert(heap->kind() == CollectedHeap::ParallelScavengeHeap, "Sanity");
  PSAdaptiveSizePolicy* size_policy = heap->size_policy();

  // The scope of casr should end after code that can change
  // CollectorPolicy::_should_clear_all_soft_refs.
  ClearedAllSoftRefs casr(clear_all_softrefs, heap->collector_policy());

  PSYoungGen* young_gen = heap->young_gen();	//年青代
  PSOldGen* old_gen = heap->old_gen();			//旧生代
  PSPermGen* perm_gen = heap->perm_gen();		//永久代

  // Increment the invocation count
  heap->increment_total_collections(true /* full */);

  // Save information needed to minimize mangling
  heap->record_gen_tops_before_GC();

  // We need to track unique mark sweep invocations as well.
  _total_invocations++;

  AdaptiveSizePolicyOutput(size_policy, heap->total_collections());

  heap->print_heap_before_gc();

  // Fill in TLABs
  heap->accumulate_statistics_all_tlabs();
  heap->ensure_parsability(true);  // retire TLABs

  if (VerifyBeforeGC && heap->total_collections() >= VerifyGCStartAt) {
    HandleMark hm;  // Discard invalid handles created during verification
    gclog_or_tty->print(" VerifyBeforeGC:");
    Universe::verify(true);
  }

  // Verify object start arrays
  if (VerifyObjectStartArray &&
      VerifyBeforeGC) {
    old_gen->verify_object_start_array();
    perm_gen->verify_object_start_array();
  }

  heap->pre_full_gc_dump();

  // Filled in below to track the state of the young gen after the collection.
  bool eden_empty;
  bool survivors_empty;
  bool young_gen_empty;

  {
    HandleMark hm;

    //本次Full Gc是否由用户调用System.gc产生
    const bool is_system_gc = gc_cause == GCCause::_java_lang_system_gc;
    // This is useful for debugging but don't change the output the
    // the customer sees.
    const char* gc_cause_str = "Full GC";
    if (is_system_gc && PrintGCDetails) {
      gc_cause_str = "Full GC (System)";
    }

    gclog_or_tty->date_stamp(PrintGC && PrintGCDateStamps);
    TraceCPUTime tcpu(PrintGCDetails, true, gclog_or_tty);
    TraceTime t1(gc_cause_str, PrintGC, !PrintGCDetails, gclog_or_tty);
    TraceCollectorStats tcs(counters());
    TraceMemoryManagerStats tms(true /* Full GC */,gc_cause);

    if (TraceGen1Time) accumulated_time()->start();

    // Let the size policy know we're starting
    size_policy->major_collection_begin();

    // When collecting the permanent generation methodOops may be moving,
    // so we either have to flush all bcp data or convert it into bci.
    CodeCache::gc_prologue();
    Threads::gc_prologue();
    BiasedLocking::preserve_marks();

    // Capture heap size before collection for printing.
    size_t prev_used = heap->used();

    // Capture perm gen size before collection for sizing.
    size_t perm_gen_prev_used = perm_gen->used_in_bytes();

    // For PrintGCDetails
    size_t old_gen_prev_used = old_gen->used_in_bytes();
    size_t young_gen_prev_used = young_gen->used_in_bytes();

    allocate_stacks();

    COMPILER2_PRESENT(DerivedPointerTable::clear());

    ref_processor()->enable_discovery(true /*verify_disabled*/, true /*verify_no_refs*/);
    ref_processor()->setup_policy(clear_all_softrefs);

    mark_sweep_phase1(clear_all_softrefs);

    mark_sweep_phase2();

    // Don't add any more derived pointers during phase3
    COMPILER2_PRESENT(assert(DerivedPointerTable::is_active(), "Sanity"));
    COMPILER2_PRESENT(DerivedPointerTable::set_active(false));

    mark_sweep_phase3();

    mark_sweep_phase4();

    restore_marks();

    deallocate_stacks();

    if (ZapUnusedHeapArea) {
      // Do a complete mangle (top to end) because the usage for
      // scratch does not maintain a top pointer.
      young_gen->to_space()->mangle_unused_area_complete();
    }

    eden_empty = young_gen->eden_space()->is_empty();
    if (!eden_empty) {
      eden_empty = absorb_live_data_from_eden(size_policy, young_gen, old_gen);
    }

    // Update heap occupancy information which is used as
    // input to soft ref clearing policy at the next gc.
    Universe::update_heap_info_at_gc();

    survivors_empty = young_gen->from_space()->is_empty() &&
                      young_gen->to_space()->is_empty();
    young_gen_empty = eden_empty && survivors_empty;

    BarrierSet* bs = heap->barrier_set();
    if (bs->is_a(BarrierSet::ModRef)) {
      ModRefBarrierSet* modBS = (ModRefBarrierSet*)bs;
      MemRegion old_mr = heap->old_gen()->reserved();
      MemRegion perm_mr = heap->perm_gen()->reserved();
      assert(perm_mr.end() <= old_mr.start(), "Generations out of order");

      if (young_gen_empty) {
        modBS->clear(MemRegion(perm_mr.start(), old_mr.end()));
      } else {
        modBS->invalidate(MemRegion(perm_mr.start(), old_mr.end()));
      }
    }

    BiasedLocking::restore_marks();
    Threads::gc_epilogue();
    CodeCache::gc_epilogue();
    JvmtiExport::gc_epilogue();

    COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

    ref_processor()->enqueue_discovered_references(NULL);

    // Update time of last GC
    reset_millis_since_last_gc();

    // Let the size policy know we're done
    size_policy->major_collection_end(old_gen->used_in_bytes(), gc_cause);

    if (UseAdaptiveSizePolicy) {

      if (PrintAdaptiveSizePolicy) {
        gclog_or_tty->print("AdaptiveSizeStart: ");
        gclog_or_tty->stamp();
        gclog_or_tty->print_cr(" collection: %d ",
                       heap->total_collections());
        if (Verbose) {
          gclog_or_tty->print("old_gen_capacity: %d young_gen_capacity: %d"
            " perm_gen_capacity: %d ",
            old_gen->capacity_in_bytes(), young_gen->capacity_in_bytes(),
            perm_gen->capacity_in_bytes());
        }
      }

      // Don't check if the size_policy is ready here.  Let
      // the size_policy check that internally.
      if (UseAdaptiveGenerationSizePolicyAtMajorCollection &&
          ((gc_cause != GCCause::_java_lang_system_gc) ||
            UseAdaptiveSizePolicyWithSystemGC)) {
        // Calculate optimal free space amounts
        assert(young_gen->max_size() >
          young_gen->from_space()->capacity_in_bytes() +
          young_gen->to_space()->capacity_in_bytes(),
          "Sizes of space in young gen are out-of-bounds");
        size_t max_eden_size = young_gen->max_size() -
          young_gen->from_space()->capacity_in_bytes() -
          young_gen->to_space()->capacity_in_bytes();
        size_policy->compute_generation_free_space(young_gen->used_in_bytes(),
                                 young_gen->eden_space()->used_in_bytes(),
                                 old_gen->used_in_bytes(),
                                 perm_gen->used_in_bytes(),
                                 young_gen->eden_space()->capacity_in_bytes(),
                                 old_gen->max_gen_size(),
                                 max_eden_size,
                                 true /* full gc*/,
                                 gc_cause,
                                 heap->collector_policy());

        heap->resize_old_gen(size_policy->calculated_old_free_size_in_bytes());

        // Don't resize the young generation at an major collection.  A
        // desired young generation size may have been calculated but
        // resizing the young generation complicates the code because the
        // resizing of the old generation may have moved the boundary
        // between the young generation and the old generation.  Let the
        // young generation resizing happen at the minor collections.
      }
      if (PrintAdaptiveSizePolicy) {
        gclog_or_tty->print_cr("AdaptiveSizeStop: collection: %d ",
                       heap->total_collections());
      }
    }

    if (UsePerfData) {
      heap->gc_policy_counters()->update_counters();
      heap->gc_policy_counters()->update_old_capacity(
        old_gen->capacity_in_bytes());
      heap->gc_policy_counters()->update_young_capacity(
        young_gen->capacity_in_bytes());
    }

    heap->resize_all_tlabs();

    // We collected the perm gen, so we'll resize it here.
    perm_gen->compute_new_size(perm_gen_prev_used);

    if (TraceGen1Time) accumulated_time()->stop();

    if (PrintGC) {
      if (PrintGCDetails) {
        // Don't print a GC timestamp here.  This is after the GC so
        // would be confusing.
        young_gen->print_used_change(young_gen_prev_used);
        old_gen->print_used_change(old_gen_prev_used);
      }
      heap->print_heap_change(prev_used);
      // Do perm gen after heap becase prev_used does
      // not include the perm gen (done this way in the other
      // collectors).
      if (PrintGCDetails) {
        perm_gen->print_used_change(perm_gen_prev_used);
      }
    }

    // Track memory usage and detect low memory
    MemoryService::track_memory_usage();
    heap->update_counters();
  }

  if (VerifyAfterGC && heap->total_collections() >= VerifyGCStartAt) {
    HandleMark hm;  // Discard invalid handles created during verification
    gclog_or_tty->print(" VerifyAfterGC:");
    Universe::verify(false);
  }

  // Re-verify object start arrays
  if (VerifyObjectStartArray &&
      VerifyAfterGC) {
    old_gen->verify_object_start_array();
    perm_gen->verify_object_start_array();
  }

  if (ZapUnusedHeapArea) {
    old_gen->object_space()->check_mangled_unused_area_complete();
    perm_gen->object_space()->check_mangled_unused_area_complete();
  }

  NOT_PRODUCT(ref_processor()->verify_no_references_recorded());

  heap->print_heap_after_gc();

  heap->post_full_gc_dump();

#ifdef TRACESPINNING
  ParallelTaskTerminator::print_termination_counts();
#endif

  return true;
}

bool PSMarkSweep::absorb_live_data_from_eden(PSAdaptiveSizePolicy* size_policy,
                                             PSYoungGen* young_gen,
                                             PSOldGen* old_gen) {
  MutableSpace* const eden_space = young_gen->eden_space();
  assert(!eden_space->is_empty(), "eden must be non-empty");
  assert(young_gen->virtual_space()->alignment() ==
         old_gen->virtual_space()->alignment(), "alignments do not match");

  if (!(UseAdaptiveSizePolicy && UseAdaptiveGCBoundary)) {
    return false;
  }

  // Both generations must be completely committed.
  if (young_gen->virtual_space()->uncommitted_size() != 0) {
    return false;
  }
  if (old_gen->virtual_space()->uncommitted_size() != 0) {
    return false;
  }

  // Figure out how much to take from eden.  Include the average amount promoted
  // in the total; otherwise the next young gen GC will simply bail out to a
  // full GC.
  const size_t alignment = old_gen->virtual_space()->alignment();
  const size_t eden_used = eden_space->used_in_bytes();
  const size_t promoted = (size_t)size_policy->avg_promoted()->padded_average();
  const size_t absorb_size = align_size_up(eden_used + promoted, alignment);
  const size_t eden_capacity = eden_space->capacity_in_bytes();

  if (absorb_size >= eden_capacity) {
    return false; // Must leave some space in eden.
  }

  const size_t new_young_size = young_gen->capacity_in_bytes() - absorb_size;
  if (new_young_size < young_gen->min_gen_size()) {
    return false; // Respect young gen minimum size.
  }

  if (TraceAdaptiveGCBoundary && Verbose) {
    gclog_or_tty->print(" absorbing " SIZE_FORMAT "K:  "
                        "eden " SIZE_FORMAT "K->" SIZE_FORMAT "K "
                        "from " SIZE_FORMAT "K, to " SIZE_FORMAT "K "
                        "young_gen " SIZE_FORMAT "K->" SIZE_FORMAT "K ",
                        absorb_size / K,
                        eden_capacity / K, (eden_capacity - absorb_size) / K,
                        young_gen->from_space()->used_in_bytes() / K,
                        young_gen->to_space()->used_in_bytes() / K,
                        young_gen->capacity_in_bytes() / K, new_young_size / K);
  }

  // Fill the unused part of the old gen.
  MutableSpace* const old_space = old_gen->object_space();
  HeapWord* const unused_start = old_space->top();
  size_t const unused_words = pointer_delta(old_space->end(), unused_start);

  if (unused_words > 0) {
    if (unused_words < CollectedHeap::min_fill_size()) {
      return false;  // If the old gen cannot be filled, must give up.
    }
    CollectedHeap::fill_with_objects(unused_start, unused_words);
  }

  // Take the live data from eden and set both top and end in the old gen to
  // eden top.  (Need to set end because reset_after_change() mangles the region
  // from end to virtual_space->high() in debug builds).
  HeapWord* const new_top = eden_space->top();
  old_gen->virtual_space()->expand_into(young_gen->virtual_space(),
                                        absorb_size);
  young_gen->reset_after_change();
  old_space->set_top(new_top);
  old_space->set_end(new_top);
  old_gen->reset_after_change();

  // Update the object start array for the filler object and the data from eden.
  ObjectStartArray* const start_array = old_gen->start_array();
  for (HeapWord* p = unused_start; p < new_top; p += oop(p)->size()) {
    start_array->allocate_block(p);
  }

  // Could update the promoted average here, but it is not typically updated at
  // full GCs and the value to use is unclear.  Something like
  //
  // cur_promoted_avg + absorb_size / number_of_scavenges_since_last_full_gc.

  size_policy->set_bytes_absorbed_from_eden(absorb_size);
  return true;
}

void PSMarkSweep::allocate_stacks() {
  ParallelScavengeHeap* heap = (ParallelScavengeHeap*)Universe::heap();
  assert(heap->kind() == CollectedHeap::ParallelScavengeHeap, "Sanity");

  PSYoungGen* young_gen = heap->young_gen();

  MutableSpace* to_space = young_gen->to_space();
  _preserved_marks = (PreservedMark*)to_space->top();
  _preserved_count = 0;

  // We want to calculate the size in bytes first.
  _preserved_count_max  = pointer_delta(to_space->end(), to_space->top(), sizeof(jbyte));
  // Now divide by the size of a PreservedMark
  _preserved_count_max /= sizeof(PreservedMark);
}


void PSMarkSweep::deallocate_stacks() {
  _preserved_mark_stack.clear(true);
  _preserved_oop_stack.clear(true);
  _marking_stack.clear();
  _objarray_stack.clear(true);
  _revisit_klass_stack.clear(true);
  _revisit_mdo_stack.clear(true);
}

/**
 * 标记堆中所有的对象
 */
void PSMarkSweep::mark_sweep_phase1(bool clear_all_softrefs) {
  // Recursively traverse all live objects and mark them
  TraceTime tm("phase 1", PrintGCDetails && Verbose, true, gclog_or_tty);
  trace(" 1");

  ParallelScavengeHeap* heap = (ParallelScavengeHeap*)Universe::heap();
  assert(heap->kind() == CollectedHeap::ParallelScavengeHeap, "Sanity");

  //标记所有的根对象
  {
    ParallelScavengeHeap::ParStrongRootsScope psrs;

    Universe::oops_do(mark_and_push_closure());
    JNIHandles::oops_do(mark_and_push_closure());   // Global (strong) JNI handles
    CodeBlobToOopClosure each_active_code_blob(mark_and_push_closure(), /*do_marking=*/ true);
    Threads::oops_do(mark_and_push_closure(), &each_active_code_blob);
    ObjectSynchronizer::oops_do(mark_and_push_closure());
    FlatProfiler::oops_do(mark_and_push_closure());
    Management::oops_do(mark_and_push_closure());
    JvmtiExport::oops_do(mark_and_push_closure());
    SystemDictionary::always_strong_oops_do(mark_and_push_closure());
    // Do not treat nmethods as strong roots for mark/sweep, since we can unload them.
    //CodeCache::scavenge_root_nmethods_do(CodeBlobToOopClosure(mark_and_push_closure()));
  }

  //迭代标记所有根对象的引用对象
  follow_stack();

  //标记找到的软/弱引用对象
  {
    ref_processor()->setup_policy(clear_all_softrefs);
    ref_processor()->process_discovered_references(
      is_alive_closure(), mark_and_push_closure(), follow_stack_closure(), NULL);
  }

  //标记所有系统词典中的对象
  bool purged_class = SystemDictionary::do_unloading(is_alive_closure());

  //标记所有代码缓存中的对象
  CodeCache::do_unloading(is_alive_closure(), mark_and_push_closure(),
                          purged_class);

  //迭代标记所有根对象的引用对象
  follow_stack(); // Flush marking stack

  //标记所有Class对象
  follow_weak_klass_links();
  assert(_marking_stack.is_empty(), "just drained");

  // Visit memoized mdo's and clear unmarked weak refs
  follow_mdo_weak_refs();
  assert(_marking_stack.is_empty(), "just drained");

  //标记所有字符串表中的对象
  StringTable::unlink(is_alive_closure());
  //标记所有符号表中的对象
  SymbolTable::unlink();

  assert(_marking_stack.is_empty(), "stack should be empty by now");
}


/**
 * 计算内存堆中live对象的新存储位置
 */
void PSMarkSweep::mark_sweep_phase2() {
  TraceTime tm("phase 2", PrintGCDetails && Verbose, true, gclog_or_tty);
  trace("2");

  // Now all live objects are marked, compute the new object addresses.

  // It is imperative that we traverse perm_gen LAST. If dead space is
  // allowed a range of dead object may get overwritten by a dead int
  // array. If perm_gen is not traversed last a klassOop may get
  // overwritten. This is fine since it is dead, but if the class has dead
  // instances we have to skip them, and in order to find their size we
  // need the klassOop!
  //
  // It is not required that we traverse spaces in the same order in
  // phase2, phase3 and phase4, but the ValidateMarkSweep live oops
  // tracking expects us to do so. See comment under phase4.

  ParallelScavengeHeap* heap = (ParallelScavengeHeap*)Universe::heap();
  assert(heap->kind() == CollectedHeap::ParallelScavengeHeap, "Sanity");

  PSOldGen* old_gen = heap->old_gen();
  PSPermGen* perm_gen = heap->perm_gen();

  // Begin compacting into the old gen
  PSMarkSweepDecorator::set_destination_decorator_tenured();

  // This will also compact the young gen spaces.
  old_gen->precompact();

  // Compact the perm gen into the perm gen
  PSMarkSweepDecorator::set_destination_decorator_perm_gen();

  perm_gen->precompact();
}

// This should be moved to the shared markSweep code!
class PSAlwaysTrueClosure: public BoolObjectClosure {
public:
  void do_object(oop p) { ShouldNotReachHere(); }
  bool do_object_b(oop p) { return true; }
};
static PSAlwaysTrueClosure always_true;

/**
 * 更新内存堆中的live对象的物理地址映射表
 */
void PSMarkSweep::mark_sweep_phase3() {
  // Adjust the pointers to reflect the new locations
  TraceTime tm("phase 3", PrintGCDetails && Verbose, true, gclog_or_tty);
  trace("3");

  ParallelScavengeHeap* heap = (ParallelScavengeHeap*)Universe::heap();
  assert(heap->kind() == CollectedHeap::ParallelScavengeHeap, "Sanity");

  PSYoungGen* young_gen = heap->young_gen();
  PSOldGen* old_gen = heap->old_gen();
  PSPermGen* perm_gen = heap->perm_gen();

  // General strong roots.
  Universe::oops_do(adjust_root_pointer_closure());
  JNIHandles::oops_do(adjust_root_pointer_closure());   // Global (strong) JNI handles
  Threads::oops_do(adjust_root_pointer_closure(), NULL);
  ObjectSynchronizer::oops_do(adjust_root_pointer_closure());
  FlatProfiler::oops_do(adjust_root_pointer_closure());
  Management::oops_do(adjust_root_pointer_closure());
  JvmtiExport::oops_do(adjust_root_pointer_closure());
  // SO_AllClasses
  SystemDictionary::oops_do(adjust_root_pointer_closure());
  //CodeCache::scavenge_root_nmethods_oops_do(adjust_root_pointer_closure());

  // Now adjust pointers in remaining weak roots.  (All of which should
  // have been cleared if they pointed to non-surviving objects.)
  // Global (weak) JNI handles
  JNIHandles::weak_oops_do(&always_true, adjust_root_pointer_closure());

  CodeCache::oops_do(adjust_pointer_closure());
  StringTable::oops_do(adjust_root_pointer_closure());
  ref_processor()->weak_oops_do(adjust_root_pointer_closure());
  PSScavenge::reference_processor()->weak_oops_do(adjust_root_pointer_closure());

  adjust_marks();

  young_gen->adjust_pointers();
  old_gen->adjust_pointers();
  perm_gen->adjust_pointers();
}

/**
 * 压缩内存堆中的live对象(移动复制live对象到新的存储位置)
 */
void PSMarkSweep::mark_sweep_phase4() {
  EventMark m("4 compact heap");
  TraceTime tm("phase 4", PrintGCDetails && Verbose, true, gclog_or_tty);
  trace("4");

  // All pointers are now adjusted, move objects accordingly

  // It is imperative that we traverse perm_gen first in phase4. All
  // classes must be allocated earlier than their instances, and traversing
  // perm_gen first makes sure that all klassOops have moved to their new
  // location before any instance does a dispatch through it's klass!
  ParallelScavengeHeap* heap = (ParallelScavengeHeap*)Universe::heap();
  assert(heap->kind() == CollectedHeap::ParallelScavengeHeap, "Sanity");

  PSYoungGen* young_gen = heap->young_gen();
  PSOldGen* old_gen = heap->old_gen();
  PSPermGen* perm_gen = heap->perm_gen();

  perm_gen->compact();
  old_gen->compact();
  young_gen->compact();
}

jlong PSMarkSweep::millis_since_last_gc() {
  // We need a monotonically non-deccreasing time in ms but
  // os::javaTimeMillis() does not guarantee monotonicity.
  jlong now = os::javaTimeNanos() / NANOSECS_PER_MILLISEC;
  jlong ret_val = now - _time_of_last_gc;
  // XXX See note in genCollectedHeap::millis_since_last_gc().
  if (ret_val < 0) {
    NOT_PRODUCT(warning("time warp: "INT64_FORMAT, ret_val);)
    return 0;
  }
  return ret_val;
}

void PSMarkSweep::reset_millis_since_last_gc() {
  // We need a monotonically non-deccreasing time in ms but
  // os::javaTimeMillis() does not guarantee monotonicity.
  _time_of_last_gc = os::javaTimeNanos() / NANOSECS_PER_MILLISEC;
}
