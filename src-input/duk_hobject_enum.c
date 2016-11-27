/*
 *  Object enumeration support.
 *
 *  Creates an internal enumeration state object to be used e.g. with for-in
 *  enumeration.  The state object contains a snapshot of target object keys
 *  and internal control state for enumeration.  Enumerator flags allow caller
 *  to e.g. request internal/non-enumerable properties, and to enumerate only
 *  "own" properties.
 *
 *  Also creates the result value for e.g. Object.keys() based on the same
 *  internal structure.
 *
 *  This snapshot-based enumeration approach is used to simplify enumeration:
 *  non-snapshot-based approaches are difficult to reconcile with mutating
 *  the enumeration target, running multiple long-lived enumerators at the
 *  same time, garbage collection details, etc.  The downside is that the
 *  enumerator object is memory inefficient especially for iterating arrays.
 */

#include "duk_internal.h"

/* XXX: identify enumeration target with an object index (not top of stack) */

/* First enumerated key index in enumerator object, must match exactly the
 * number of control properties inserted to the enumerator.
 */
#define DUK__ENUM_START_INDEX  2

DUK_LOCAL const duk_uint16_t duk__bufobj_virtual_props[] = {
	DUK_STRIDX_LENGTH,
	DUK_STRIDX_BYTE_LENGTH,
	DUK_STRIDX_BYTE_OFFSET,
	DUK_STRIDX_BYTES_PER_ELEMENT
};

#if 0
/* Current implementation suffices for ES6 for now because there's no symbol
 * sorting, so commented out for now.
 */

/*
 *  Helper to sort enumeration keys using a callback for pairwise duk_hstring
 *  comparisons.  The keys are in the enumeration object entry part, starting
 *  from DUK__ENUM_START_INDEX, and the entry part is dense.  Entry part values
 *  are all "true", e.g. "1" -> true, "3" -> true, "foo" -> true "2" -> true,
 *  so it suffices to just switch keys without switching values.
 *
 *  Insertion sort is used because (1) it's simple and compact, (2) works
 *  in-place, (3) minimizes operations if data is already nearly sorted,
 *  (4) doesn't reorder elements considered equal.
 *  http://en.wikipedia.org/wiki/Insertion_sort
 */

typedef duk_bool_t (*duk__sort_compare_fn)(duk_hstring *a, duk_hstring *b, duk_uarridx_t val_b);

DUK_LOCAL duk_bool_t duk__sort_compare_es6(duk_hstring *a, duk_hstring *b, duk_uarridx_t val_b) {
	duk_uarridx_t val_a;

	DUK_ASSERT(a != NULL);
	DUK_ASSERT(b != NULL);
	DUK_UNREF(b);  /* Not actually needed now, val_b suffices. */

	/* ES6 [[OwnPropertyKeys]] enumeration order for ordinary objects:
	 * (1) array indices in ascending order, (2) non-array-index keys in
	 * insertion order, symbols in insertion order:
	 * http://www.ecma-international.org/ecma-262/6.0/#sec-ordinary-object-internal-methods-and-internal-slots-ownpropertykeys.
	 *
	 * This rule is applied to "own properties" at each inheritance level;
	 * non-duplicate parent keys always follow child keys.  For example,
	 * an inherited array index will enumerate -after- a symbol in the
	 * child.
	 */

	val_a = DUK_HSTRING_GET_ARRIDX_FAST(a);

	if (val_b < val_a) {
		/* Covers:
		 *   - Both keys are array indices and a > b: don't insert here.
		 *   - 'b' is array index, 'a' is not: don't insert here.
		 */
		return 0;
	} else {
		/* Covers:
		 *   val_a < val_b where:
		 *   - Both keys are array indices and a < b: insert here.
		 *   - 'a' is array index, 'b' is not: insert here.
		 *   val_a == val_b where:
		 *   - Both keys are array indices and a == b: insert here
		 *     (shouldn't actually happen, can't have non-duplicate
		 *     identical array index keys).
		 *   - Neither key is an array index: insert here, keeps key
		 *     order regardless of the keys themselves.
		 */
		return 1;
	}
}

DUK_LOCAL void duk__sort_enum_keys_es6(duk_hthread *thr, duk_hobject *h_obj, duk_int_fast32_t idx_start, duk_int_fast32_t idx_end) {
	duk_hstring **keys;
	duk_int_fast32_t idx;

	DUK_ASSERT(h_obj != NULL);
	DUK_ASSERT(idx_start >= DUK__ENUM_START_INDEX);
	DUK_ASSERT(idx_end >= idx_start);
	DUK_UNREF(thr);

	if (idx_end <= idx_start + 1) {
		return;  /* Zero or one element(s). */
	}

	keys = DUK_HOBJECT_E_GET_KEY_BASE(thr->heap, h_obj);

	for (idx = idx_start + 1; idx < idx_end; idx++) {
		duk_hstring *h_curr;
		duk_int_fast32_t idx_insert;
		duk_uarridx_t val_curr;

		h_curr = keys[idx];
		DUK_ASSERT(h_curr != NULL);

		/* Scan backwards for insertion place.  This works very well
		 * when the elements are nearly in order which is the common
		 * (and optimized for) case.
		 */

		val_curr = DUK_HSTRING_GET_ARRIDX_FAST(h_curr);  /* Remains same during scanning. */
		for (idx_insert = idx - 1; idx_insert >= idx_start; idx_insert--) {
			duk_hstring *h_insert;
			h_insert = keys[idx_insert];
			DUK_ASSERT(h_insert != NULL);

			/* XXX: fixed callback rather than a callback argument; only
			 * one argument used and using a callback argument doesn't
			 * cause e.g. gcc to inline the callback.
			 */
			if (duk__sort_compare_es6(h_insert, h_curr, val_curr)) {
				break;
			}
		}
		/* If we're out of indices, idx_insert == idx_start - 1 and idx_insert++
		 * brings us back to idx_start.
		 */
		idx_insert++;
		DUK_ASSERT(idx_insert >= 0 && idx_insert <= idx);

		/*        .-- p_insert   .-- p_curr
		 *        v              v
		 *  | ... | insert | ... | curr
		 */

		/* This could also done when the keys are in order, i.e.
		 * idx_insert == idx.  The result would be an unnecessary
		 * memmove() but we use an explicit check because the keys
		 * are very often in order already.
		 */
		if (idx != idx_insert) {
			DUK_MEMMOVE((void *) (keys + idx_insert + 1),
			            (const void *) (keys + idx_insert),
			            (size_t) ((idx - idx_insert) * sizeof(duk_hstring *)));
			keys[idx_insert] = h_curr;
		}
	}
}
#endif  /* disabled */

/*
 *  Helper to sort keys into ES6 [[OwnPropertyKeys]] enumeration order:
 *  array keys in ascending order first, followed by keys in insertion
 *  order, followed by symbols in insertion order (not handled here).
 *  Insertion sort based.
 *
 *  This algorithm nominally sorts array indices, but because the "no array
 *  index" marker is higher than any array index, non-array-index keys are
 *  sorted after array indices.  Non-array-index keys are also considered
 *  equal for sorting which means that their order is kept as is, so the end
 *  result matches ES6 [[OwnPropertyKeys]].
 *
 *  Insertion sort is used because (1) it's simple and compact, (2) works
 *  in-place, (3) minimizes operations if data is already nearly sorted,
 *  (4) doesn't reorder elements considered equal.
 *  http://en.wikipedia.org/wiki/Insertion_sort
 */

DUK_LOCAL void duk__sort_enum_keys_es6(duk_hthread *thr, duk_hobject *h_obj, duk_int_fast32_t idx_start, duk_int_fast32_t idx_end) {
	duk_hstring **keys;
	duk_hstring **p_curr, **p_insert, **p_end;
	duk_hstring *h_curr;
	duk_uarridx_t val_highest, val_curr, val_insert;

	DUK_ASSERT(h_obj != NULL);
	DUK_ASSERT(idx_start >= DUK__ENUM_START_INDEX);
	DUK_ASSERT(idx_end >= idx_start);
	DUK_UNREF(thr);

	if (idx_end <= idx_start + 1) {
		return;  /* Zero or one element(s). */
	}

	keys = DUK_HOBJECT_E_GET_KEY_BASE(thr->heap, h_obj);
	p_curr = keys + idx_start;
	val_highest = DUK_HSTRING_GET_ARRIDX_SLOW(*p_curr);
	for (p_curr++, p_end = keys + idx_end; p_curr < p_end; p_curr++) {
		DUK_ASSERT(*p_curr != NULL);
		val_curr = DUK_HSTRING_GET_ARRIDX_SLOW(*p_curr);

		if (val_curr >= val_highest) {
			val_highest = val_curr;
			continue;
		}

		/* Needs to be inserted; scan backwards, since we optimize
		 * for the case where elements are nearly in order.
		 */

		p_insert = p_curr;
		for (;;) {
			p_insert--;  /* Start from p_curr - 1. */
			val_insert = DUK_HSTRING_GET_ARRIDX_SLOW(*p_insert);
			if (val_insert < val_curr) {
				p_insert++;
				break;
			}
			if (p_insert == keys + idx_start) {
				break;
			}
		}

		/*        .-- p_insert   .-- p_curr
		 *        v              v
		 *  | ... | insert | ... | curr
		 */

		h_curr = *p_curr;
		DUK_MEMMOVE((void *) (p_insert + 1),
		            (const void *) p_insert,
		            (size_t) ((p_curr - p_insert) * sizeof(duk_hstring *)));
		*p_insert = h_curr;
		/* keep val_highest */
	}
}

/*
 *  Create an internal enumerator object E, which has its keys ordered
 *  to match desired enumeration ordering.  Also initialize internal control
 *  properties for enumeration.
 *
 *  Note: if an array was used to hold enumeration keys instead, an array
 *  scan would be needed to eliminate duplicates found in the prototype chain.
 */

DUK_LOCAL void duk__add_enum_key(duk_context *ctx, duk_hstring *k) {
	duk_push_hstring(ctx, k);
	duk_push_true(ctx);
	duk_put_prop(ctx, -3);
}

DUK_LOCAL void duk__add_enum_key_stridx(duk_context *ctx, duk_small_uint_t stridx) {
	duk__add_enum_key(ctx, DUK_HTHREAD_GET_STRING((duk_hthread *) ctx, stridx));
}

DUK_INTERNAL void duk_hobject_enumerator_create(duk_context *ctx, duk_small_uint_t enum_flags) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_hobject *enum_target;
	duk_hobject *curr;
	duk_hobject *res;
#if defined(DUK_USE_ES6_PROXY)
	duk_hobject *h_proxy_target;
	duk_hobject *h_proxy_handler;
	duk_hobject *h_trap_result;
#endif
	duk_uint_fast32_t i, len;  /* used for array, stack, and entry indices */
	duk_uint_fast32_t sort_start_index;

	DUK_ASSERT(ctx != NULL);

	enum_target = duk_require_hobject(ctx, -1);
	DUK_ASSERT(enum_target != NULL);

	duk_push_object_internal(ctx);
	res = duk_known_hobject(ctx, -1);

	/* [enum_target res] */

	/* Target must be stored so that we can recheck whether or not
	 * keys still exist when we enumerate.  This is not done if the
	 * enumeration result comes from a proxy trap as there is no
	 * real object to check against.
	 */
	duk_push_hobject(ctx, enum_target);
	duk_put_prop_stridx(ctx, -2, DUK_STRIDX_INT_TARGET);

	/* Initialize index so that we skip internal control keys. */
	duk_push_int(ctx, DUK__ENUM_START_INDEX);
	duk_put_prop_stridx(ctx, -2, DUK_STRIDX_INT_NEXT);

	/*
	 *  Proxy object handling
	 */

#if defined(DUK_USE_ES6_PROXY)
	if (DUK_LIKELY((enum_flags & DUK_ENUM_NO_PROXY_BEHAVIOR) != 0)) {
		goto skip_proxy;
	}
	if (DUK_LIKELY(!duk_hobject_proxy_check(thr,
	                                        enum_target,
	                                        &h_proxy_target,
	                                        &h_proxy_handler))) {
		goto skip_proxy;
	}

	/* XXX: share code with Object.keys() Proxy handling */

	/* In ES6 for-in invoked the "enumerate" trap; in ES7 "enumerate"
	 * has been obsoleted and "ownKeys" is used instead.
	 */
	DUK_DDD(DUK_DDDPRINT("proxy enumeration"));
	duk_push_hobject(ctx, h_proxy_handler);
	if (!duk_get_prop_stridx(ctx, -1, DUK_STRIDX_OWN_KEYS)) {
		/* No need to replace the 'enum_target' value in stack, only the
		 * enum_target reference.  This also ensures that the original
		 * enum target is reachable, which keeps the proxy and the proxy
		 * target reachable.  We do need to replace the internal _Target.
		 */
		DUK_DDD(DUK_DDDPRINT("no ownKeys trap, enumerate proxy target instead"));
		DUK_DDD(DUK_DDDPRINT("h_proxy_target=%!O", (duk_heaphdr *) h_proxy_target));
		enum_target = h_proxy_target;

		duk_push_hobject(ctx, enum_target);  /* -> [ ... enum_target res handler undefined target ] */
		duk_put_prop_stridx(ctx, -4, DUK_STRIDX_INT_TARGET);

		duk_pop_2(ctx);  /* -> [ ... enum_target res ] */
		goto skip_proxy;
	}

	/* [ ... enum_target res handler trap ] */
	duk_insert(ctx, -2);
	duk_push_hobject(ctx, h_proxy_target);    /* -> [ ... enum_target res trap handler target ] */
	duk_call_method(ctx, 1 /*nargs*/);        /* -> [ ... enum_target res trap_result ] */
	h_trap_result = duk_require_hobject(ctx, -1);
	DUK_UNREF(h_trap_result);

	/* XXX: more filter flags? */
	duk_proxy_ownkeys_postprocess(ctx, h_proxy_target, (enum_flags & DUK_ENUM_INCLUDE_NONENUMERABLE) ? 0 : 1 /*enumerable_only*/);
	/* -> [ ... enum_target res trap_result keys_array ] */

	/* Copy cleaned up trap result keys into the enumerator object. */
	/* XXX: result is a dense array; could make use of that. */
	DUK_ASSERT(duk_is_array(ctx, -1));
	len = (duk_uint_fast32_t) duk_get_length(ctx, -1);
	for (i = 0; i < len; i++) {
		(void) duk_get_prop_index(ctx, -1, i);
		DUK_ASSERT(duk_is_string(ctx, -1));  /* postprocess cleaned up */
		/* [ ... enum_target res trap_result keys_array val ] */
		duk_push_true(ctx);
		/* [ ... enum_target res trap_result keys_array val true ] */
		duk_put_prop(ctx, -5);
	}
	/* [ ... enum_target res trap_result keys_array ] */
	duk_pop_2(ctx);
	duk_remove(ctx, -2);

	/* [ ... res ] */

	/* The internal _Target property is kept pointing to the original
	 * enumeration target (the proxy object), so that the enumerator
	 * 'next' operation can read property values if so requested.  The
	 * fact that the _Target is a proxy disables key existence check
	 * during enumeration.
	 */
	DUK_DDD(DUK_DDDPRINT("proxy enumeration, final res: %!O", (duk_heaphdr *) res));
	goto compact_and_return;

 skip_proxy:
#endif  /* DUK_USE_ES6_PROXY */

	curr = enum_target;
	sort_start_index = DUK__ENUM_START_INDEX;
	DUK_ASSERT(DUK_HOBJECT_GET_ENEXT(res) == DUK__ENUM_START_INDEX);
	while (curr) {
		duk_uint_fast32_t sort_end_index;
#if !defined(DUK_USE_PREFER_SIZE)
		duk_bool_t need_sort = 0;
#endif

		/* Enumeration proceeds by inheritance level.  Virtual
		 * properties need to be handled specially, followed by
		 * array part, and finally entry part.
		 *
		 * If there are array index keys in the entry part or any
		 * other risk of the ES6 [[OwnPropertyKeys]] order being
		 * violated, need_sort is set and an explicit ES6 sort is
		 * done for the inheritance level.
		 */

		/* XXX: inheriting from proxy */

		/*
		 *  Virtual properties.
		 *
		 *  String and buffer indices are virtual and always enumerable,
		 *  'length' is virtual and non-enumerable.  Array and arguments
		 *  object props have special behavior but are concrete.
		 *
		 *  String and buffer objects don't have an array part so as long
		 *  as virtual array index keys are enumerated first, we don't
		 *  need to set need_sort.
		 */

#if defined(DUK_USE_BUFFEROBJECT_SUPPORT)
		if (DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(curr) || DUK_HOBJECT_IS_BUFOBJ(curr)) {
#else
		if (DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(curr)) {
#endif
			/* String and buffer enumeration behavior is identical now,
			 * so use shared handler.
			 */
			if (DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(curr)) {
				duk_hstring *h_val;
				h_val = duk_hobject_get_internal_value_string(thr->heap, curr);
				DUK_ASSERT(h_val != NULL);  /* string objects must not created without internal value */
				len = (duk_uint_fast32_t) DUK_HSTRING_GET_CHARLEN(h_val);
			}
#if defined(DUK_USE_BUFFEROBJECT_SUPPORT)
			else {
				duk_hbufobj *h_bufobj;
				DUK_ASSERT(DUK_HOBJECT_IS_BUFOBJ(curr));
				h_bufobj = (duk_hbufobj *) curr;
				if (h_bufobj == NULL ||
				   (DUK_HOBJECT_GET_CLASS_NUMBER(curr) == DUK_HOBJECT_CLASS_ARRAYBUFFER &&
				    ((enum_flags & DUK_ENUM_INCLUDE_NONENUMERABLE) == 0))) {
					/* Zero length seems like a good behavior for neutered buffers.
					 * ArrayBuffer index properties are non-enumerable, so use len = 0
					 * to ensure they're not enumerated (unless requested explicitly).
					 */
					len = 0;
				} else {
					/* There's intentionally no check for
					 * current underlying buffer length.
					 */
					len = (duk_uint_fast32_t) (h_bufobj->length >> h_bufobj->shift);
				}
			}
#endif  /* DUK_USE_BUFFEROBJECT_SUPPORT */

			for (i = 0; i < len; i++) {
				duk_hstring *k;

				/* This is a bit fragile: the string is not
				 * reachable until it is pushed by the helper.
				 */
				k = duk_heap_string_intern_u32_checked(thr, i);
				DUK_ASSERT(k);

				duk__add_enum_key(ctx, k);

				/* [enum_target res] */
			}

			/* 'length' and other virtual properties are not
			 * enumerable, but are included if non-enumerable
			 * properties are requested.
			 */

			if (enum_flags & DUK_ENUM_INCLUDE_NONENUMERABLE) {
				duk_uint_fast32_t n;

#if defined(DUK_USE_BUFFEROBJECT_SUPPORT)
				if (DUK_HOBJECT_IS_BUFOBJ(curr)) {
					n = sizeof(duk__bufobj_virtual_props) / sizeof(duk_uint16_t);
				}
				else
#endif
				{
					DUK_ASSERT(DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(curr));
					DUK_ASSERT(duk__bufobj_virtual_props[0] == DUK_STRIDX_LENGTH);
					n = 1;  /* only 'length' */
				}

				for (i = 0; i < n; i++) {
					duk__add_enum_key_stridx(ctx, duk__bufobj_virtual_props[i]);
				}

			}
		} else if (DUK_HOBJECT_HAS_EXOTIC_DUKFUNC(curr)) {
			if (enum_flags & DUK_ENUM_INCLUDE_NONENUMERABLE) {
				duk__add_enum_key_stridx(ctx, DUK_STRIDX_LENGTH);
			}
		}

		/*
		 *  Array part
		 */

		for (i = 0; i < (duk_uint_fast32_t) DUK_HOBJECT_GET_ASIZE(curr); i++) {
			duk_hstring *k;
			duk_tval *tv;

			tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, curr, i);
			if (DUK_TVAL_IS_UNUSED(tv)) {
				continue;
			}
			k = duk_heap_string_intern_u32_checked(thr, i);  /* Fragile reachability. */
			DUK_ASSERT(k);

			duk__add_enum_key(ctx, k);

			/* [enum_target res] */
		}

		if (DUK_HOBJECT_HAS_EXOTIC_ARRAY(curr)) {
			/* Array .length comes after numeric indices. */
			if (enum_flags & DUK_ENUM_INCLUDE_NONENUMERABLE) {
				duk__add_enum_key_stridx(ctx, DUK_STRIDX_LENGTH);
			}
		}

		/*
		 *  Entries part
		 */

		for (i = 0; i < (duk_uint_fast32_t) DUK_HOBJECT_GET_ENEXT(curr); i++) {
			duk_hstring *k;

			k = DUK_HOBJECT_E_GET_KEY(thr->heap, curr, i);
			if (!k) {
				continue;
			}
			if (!(enum_flags & DUK_ENUM_INCLUDE_NONENUMERABLE) &&
			    !DUK_HOBJECT_E_SLOT_IS_ENUMERABLE(thr->heap, curr, i)) {
				continue;
			}
			if (!(enum_flags & DUK_ENUM_INCLUDE_INTERNAL) &&
			    DUK_HSTRING_HAS_INTERNAL(k)) {
				continue;
			}
			if (DUK_HSTRING_HAS_ARRIDX(k)) {
				/* This in currently only possible if the
				 * object has no array part: the array part
				 * is exhaustive when it is present.
				 */
#if !defined(DUK_USE_PREFER_SIZE)
				need_sort = 1;
#endif
			} else {
				if (enum_flags & DUK_ENUM_ARRAY_INDICES_ONLY) {
					continue;
				}
			}

			DUK_ASSERT(DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, curr, i) ||
			           !DUK_TVAL_IS_UNUSED(&DUK_HOBJECT_E_GET_VALUE_PTR(thr->heap, curr, i)->v));

			duk__add_enum_key(ctx, k);

			/* [enum_target res] */
		}

		/* Sort enumerated keys according to ES6 requirements for
		 * the "inheritance level" just processed.  This is far from
		 * optimal, ES6 semantics could be achieved more efficiently
		 * by handling array index string keys (and symbol keys)
		 * specially above in effect doing the sort inline.
		 *
		 * Skip the sort if array index sorting is requested because
		 * we must consider all keys, also inherited, so an explicit
		 * sort is done for the whole result after we're done with the
		 * prototype chain.
		 *
		 * Also skip the sort if need_sort == 0, i.e. we know for
		 * certain that the enumerated order is already correct.
		 */
		sort_end_index = DUK_HOBJECT_GET_ENEXT(res);

		if (!(enum_flags & DUK_ENUM_SORT_ARRAY_INDICES)) {
#if defined(DUK_USE_PREFER_SIZE)
			duk__sort_enum_keys_es6(thr, res, sort_start_index, sort_end_index);
#else
			if (need_sort) {
				DUK_DDD(DUK_DDDPRINT("need to sort"));
				duk__sort_enum_keys_es6(thr, res, sort_start_index, sort_end_index);
			} else {
				DUK_DDD(DUK_DDDPRINT("no need to sort"));
			}
#endif
		}

		sort_start_index = sort_end_index;

		if (enum_flags & DUK_ENUM_OWN_PROPERTIES_ONLY) {
			break;
		}

		curr = DUK_HOBJECT_GET_PROTOTYPE(thr->heap, curr);
	}

	/* [enum_target res] */

	duk_remove(ctx, -2);

	/* [res] */

	if (enum_flags & DUK_ENUM_SORT_ARRAY_INDICES) {
		/* Some E5/E5.1 algorithms require that array indices are iterated
		 * in a strictly ascending order.  This is the case for e.g.
		 * Array.prototype.forEach() and JSON.stringify() PropertyList
		 * handling.  The caller can request an explicit sort in these
		 * cases.
		 */

		/* Sort to ES6 order which works for pure array incides but
		 * also for mixed keys.
		 */
		duk__sort_enum_keys_es6(thr, res, DUK__ENUM_START_INDEX, DUK_HOBJECT_GET_ENEXT(res));
	}

#if defined(DUK_USE_ES6_PROXY)
 compact_and_return:
#endif
	/* compact; no need to seal because object is internal */
	duk_hobject_compact_props(thr, res);

	DUK_DDD(DUK_DDDPRINT("created enumerator object: %!iT", (duk_tval *) duk_get_tval(ctx, -1)));
}

/*
 *  Returns non-zero if a key and/or value was enumerated, and:
 *
 *   [enum] -> [key]        (get_value == 0)
 *   [enum] -> [key value]  (get_value == 1)
 *
 *  Returns zero without pushing anything on the stack otherwise.
 */
DUK_INTERNAL duk_bool_t duk_hobject_enumerator_next(duk_context *ctx, duk_bool_t get_value) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_hobject *e;
	duk_hobject *enum_target;
	duk_hstring *res = NULL;
	duk_uint_fast32_t idx;
	duk_bool_t check_existence;

	DUK_ASSERT(ctx != NULL);

	/* [... enum] */

	e = duk_require_hobject(ctx, -1);

	/* XXX use get tval ptr, more efficient */
	duk_get_prop_stridx(ctx, -1, DUK_STRIDX_INT_NEXT);
	idx = (duk_uint_fast32_t) duk_require_uint(ctx, -1);
	duk_pop(ctx);
	DUK_DDD(DUK_DDDPRINT("enumeration: index is: %ld", (long) idx));

	/* Enumeration keys are checked against the enumeration target (to see
	 * that they still exist).  In the proxy enumeration case _Target will
	 * be the proxy, and checking key existence against the proxy is not
	 * required (or sensible, as the keys may be fully virtual).
	 */
	duk_get_prop_stridx(ctx, -1, DUK_STRIDX_INT_TARGET);
	enum_target = duk_require_hobject(ctx, -1);
	DUK_ASSERT(enum_target != NULL);
#if defined(DUK_USE_ES6_PROXY)
	check_existence = (!DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(enum_target));
#else
	check_existence = 1;
#endif
	duk_pop(ctx);  /* still reachable */

	DUK_DDD(DUK_DDDPRINT("getting next enum value, enum_target=%!iO, enumerator=%!iT",
	                     (duk_heaphdr *) enum_target, (duk_tval *) duk_get_tval(ctx, -1)));

	/* no array part */
	for (;;) {
		duk_hstring *k;

		if (idx >= DUK_HOBJECT_GET_ENEXT(e)) {
			DUK_DDD(DUK_DDDPRINT("enumeration: ran out of elements"));
			break;
		}

		/* we know these because enum objects are internally created */
		k = DUK_HOBJECT_E_GET_KEY(thr->heap, e, idx);
		DUK_ASSERT(k != NULL);
		DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, e, idx));
		DUK_ASSERT(!DUK_TVAL_IS_UNUSED(&DUK_HOBJECT_E_GET_VALUE(thr->heap, e, idx).v));

		idx++;

		/* recheck that the property still exists */
		if (check_existence && !duk_hobject_hasprop_raw(thr, enum_target, k)) {
			DUK_DDD(DUK_DDDPRINT("property deleted during enumeration, skip"));
			continue;
		}

		DUK_DDD(DUK_DDDPRINT("enumeration: found element, key: %!O", (duk_heaphdr *) k));
		res = k;
		break;
	}

	DUK_DDD(DUK_DDDPRINT("enumeration: updating next index to %ld", (long) idx));

	duk_push_u32(ctx, (duk_uint32_t) idx);
	duk_put_prop_stridx(ctx, -2, DUK_STRIDX_INT_NEXT);

	/* [... enum] */

	if (res) {
		duk_push_hstring(ctx, res);
		if (get_value) {
			duk_push_hobject(ctx, enum_target);
			duk_dup_m2(ctx);       /* -> [... enum key enum_target key] */
			duk_get_prop(ctx, -2); /* -> [... enum key enum_target val] */
			duk_remove(ctx, -2);   /* -> [... enum key val] */
			duk_remove(ctx, -3);   /* -> [... key val] */
		} else {
			duk_remove(ctx, -2);   /* -> [... key] */
		}
		return 1;
	} else {
		duk_pop(ctx);  /* -> [...] */
		return 0;
	}
}

/*
 *  Get enumerated keys in an Ecmascript array.  Matches Object.keys() behavior
 *  described in E5 Section 15.2.3.14.
 */

DUK_INTERNAL duk_ret_t duk_hobject_get_enumerated_keys(duk_context *ctx, duk_small_uint_t enum_flags) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_hobject *e;
	duk_harray *a;
	duk_hstring **keys;
	duk_tval *tv;
	duk_uint_fast32_t count;

	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(duk_get_hobject(ctx, -1) != NULL);
	DUK_UNREF(thr);

	/* Create a temporary enumerator to get the (non-duplicated) key list;
	 * the enumerator state is initialized without being needed, but that
	 * has little impact.
	 */

	duk_hobject_enumerator_create(ctx, enum_flags);
	e = duk_known_hobject(ctx, -1);

	/* [enum_target enum res] */

	/* Create dense result array to exact size. */
	DUK_ASSERT(DUK_HOBJECT_GET_ENEXT(e) >= DUK__ENUM_START_INDEX);
	count = (duk_uint32_t) (DUK_HOBJECT_GET_ENEXT(e) - DUK__ENUM_START_INDEX);

	a = duk_push_harray_with_size(ctx, count);
	DUK_ASSERT(a != NULL);
	DUK_ASSERT(DUK_HOBJECT_GET_ASIZE((duk_hobject *) a) == count);
	DUK_ASSERT(a->length == count);
	tv = DUK_HOBJECT_A_GET_BASE(thr->heap, (duk_hobject *) a);

	/* Fill result array, no side effects. */

	keys = DUK_HOBJECT_E_GET_KEY_BASE(thr->heap, e);
	keys += DUK__ENUM_START_INDEX;

	while (count-- > 0) {
		duk_hstring *k;

		k = *keys++;
		DUK_ASSERT(k != NULL);  /* enumerator must have no keys deleted */

		DUK_TVAL_SET_STRING(tv, k);
		tv++;
		DUK_HSTRING_INCREF(thr, k);
	}

	/* [enum_target enum res] */
	duk_remove(ctx, -2);

	/* [enum_target res] */

	return 1;  /* return 1 to allow callers to tail call */
}
