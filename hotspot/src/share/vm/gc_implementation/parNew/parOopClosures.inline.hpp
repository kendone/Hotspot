/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_PARNEW_PAROOPCLOSURES_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_PARNEW_PAROOPCLOSURES_INLINE_HPP

#include "gc_implementation/parNew/parNewGeneration.hpp"
#include "gc_implementation/parNew/parOopClosures.hpp"
#include "memory/cardTableRS.hpp"

template <class T> inline void ParScanWeakRefClosure::do_oop_work(T* p) {
  assert (!oopDesc::is_null(*p), "null weak reference?");
  oop obj = oopDesc::load_decode_heap_oop_not_null(p);
  // weak references are sometimes scanned twice; must check
  // that to-space doesn't already contain this object
  if ((HeapWord*)obj < _boundary && !_g->to()->is_in_reserved(obj)) {
    // we need to ensure that it is copied (see comment in
    // ParScanClosure::do_oop_work).
    klassOop objK = obj->klass();
    markOop m = obj->mark();
    oop new_obj;
    if (m->is_marked()) { // Contains forwarding pointer.
      new_obj = ParNewGeneration::real_forwardee(obj);
    } else {
      size_t obj_sz = obj->size_given_klass(objK->klass_part());
      new_obj = ((ParNewGeneration*)_g)->copy_to_survivor_space(_par_scan_state,
                                                                obj, obj_sz, m);
    }
    oopDesc::encode_store_heap_oop_not_null(p, new_obj);
  }
}

inline void ParScanWeakRefClosure::do_oop_nv(oop* p)       { ParScanWeakRefClosure::do_oop_work(p); }
inline void ParScanWeakRefClosure::do_oop_nv(narrowOop* p) { ParScanWeakRefClosure::do_oop_work(p); }

template <class T> inline void ParScanClosure::par_do_barrier(T* p) {
  assert(generation()->is_in_reserved(p), "expected ref in generation");
  assert(!oopDesc::is_null(*p), "expected non-null object");
  oop obj = oopDesc::load_decode_heap_oop_not_null(p);
  // If p points to a younger generation, mark the card.
  if ((HeapWord*)obj < gen_boundary()) {
    rs()->write_ref_field_gc_par(p, obj);
  }
}

/**
 * 以扫描复制的方式回收垃圾对象
 */
template <class T>
inline void ParScanClosure::do_oop_work(T* p, bool gc_barrier, bool root_scan) {
  assert((!Universe::heap()->is_in_reserved(p) ||
          generation()->is_in_reserved(p))
         && (generation()->level() == 0 || gc_barrier),
         "The gen must be right, and we must be doing the barrier in older generations.");

  T heap_oop = oopDesc::load_heap_oop(p);
  if (!oopDesc::is_null(heap_oop)) {
    oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);

    if ((HeapWord*)obj < _boundary) {	//对象在当前的内存代中
#ifndef PRODUCT
      if (_g->to()->is_in_reserved(obj)) {	//待复制的对象已在To区,这种情况在正常情况下是不可能发生的
        tty->print_cr("Scanning field (" PTR_FORMAT ") twice?", p);
        GenCollectedHeap* gch =  (GenCollectedHeap*)Universe::heap();
        Space* sp = gch->space_containing(p);
        oop obj = oop(sp->block_start(p));
        assert((HeapWord*)obj < (HeapWord*)p, "Error");
        tty->print_cr("Object: " PTR_FORMAT, obj);
        tty->print_cr("-------");
        obj->print();
        tty->print_cr("-----");
        tty->print_cr("Heap:");
        tty->print_cr("-----");
        gch->print();
        ShouldNotReachHere();
      }
#endif
      // OK, we need to ensure that it is copied.
      // We read the klass and mark in this order, so that we can reliably
      // get the size of the object: if the mark we read is not a
      // forwarding pointer, then the klass is valid: the klass is only
      // overwritten with an overflow next pointer after the object is
      // forwarded.
      klassOop objK = obj->klass();
      markOop m = obj->mark();
      oop new_obj;
      if (m->is_marked()) { //待复制的对象已经被其它Gc线程处理了
        new_obj = ParNewGeneration::real_forwardee(obj);	//解析对象的物理存储地址
        //更新对象的地址映射表
        oopDesc::encode_store_heap_oop_not_null(p, new_obj);
      } else {
    	//获取对象所占存储空间的大小
        size_t obj_sz = obj->size_given_klass(objK->klass_part());
        //复制对象到新的存储空间
        new_obj = _g->copy_to_survivor_space(_par_scan_state, obj, obj_sz, m);

        //更新对象的地址映射表
        oopDesc::encode_store_heap_oop_not_null(p, new_obj);

        if (root_scan) {//复制对象是根对象
          // This may have pushed an object.  If we have a root
          // category with a lot of roots, can't let the queue get too
          // full:
          (void)_par_scan_state->trim_queues(10 * ParallelGCThreads);
        }
      }

      if (gc_barrier) {
        // Now call parent closure
        par_do_barrier(p);
      }
    }
  }
}

inline void ParScanWithBarrierClosure::do_oop_nv(oop* p)       { ParScanClosure::do_oop_work(p, true, false); }
inline void ParScanWithBarrierClosure::do_oop_nv(narrowOop* p) { ParScanClosure::do_oop_work(p, true, false); }

inline void ParScanWithoutBarrierClosure::do_oop_nv(oop* p)       { ParScanClosure::do_oop_work(p, false, false); }
inline void ParScanWithoutBarrierClosure::do_oop_nv(narrowOop* p) { ParScanClosure::do_oop_work(p, false, false); }

#endif // SHARE_VM_GC_IMPLEMENTATION_PARNEW_PAROOPCLOSURES_INLINE_HPP
