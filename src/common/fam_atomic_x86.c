/*
 *  (c) Copyright 2016-2021 Hewlett Packard Enterprise Development Company LP.
 *
 *  Author: Jason Low <jason.low2@hpe.com>
 *
 *  This software is available to you under a choice of one of two
 *  licenses. You may choose to be licensed under the terms of the 
 *  GNU Lesser General Public License Version 3, or (at your option)  
 *  later with exceptions included below, or under the terms of the  
 *  MIT license (Expat) available in COPYING file in the source tree.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As an exception, the copyright holders of this Library grant you permission
 *  to (i) compile an Application with the Library, and (ii) distribute the
 *  Application containing code generated by the Library and added to the
 *  Application during this compilation process under terms of your choice,
 *  provided you also meet the terms and conditions of the Application license.
 *
 */

//#include <stdatomic.h>
//#include <atomic.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvmm/fam_atomic_x86.h"

#define LOCK_PREFIX_HERE                  \
	".pushsection .smp_locks,\"a\"\n" \
	".balign 4\n"                     \
	".long 671f - .\n"                \
	".popsection\n"                   \
	"671:"

#define LOCK_PREFIX LOCK_PREFIX_HERE "\n\tlock; "

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#define debug(message)

#define u32 unsigned int
#define u64 unsigned long long

void fam_atomic_compare_exchange_wrong_size(void);
void fam_atomic_xadd_wrong_size(void);
void fam_atomic_xchg_wrong_size(void);
void store_release_wrong_size(void);
void fam_atomic_arch_not_supported(void);

#define store_release(ptr, val) 		\
do {						\
	__asm__ __volatile__("": : :"memory");	\
	ACCESS_ONCE(*ptr) =  val;		\
} while (0);

#define __x86_raw_cmpxchg(ptr, old, new, size, lock)		\
({								\
	__typeof__(*(ptr)) __ret;				\
	__typeof__(*(ptr)) __old = (old);			\
	__typeof__(*(ptr)) __new = (new);			\
	switch (size) {						\
	case 4:							\
	{							\
		volatile u32 *__ptr = (volatile u32 *)(ptr);	\
		asm volatile(lock "cmpxchgl %2,%1"		\
			     : "=a" (__ret), "+m" (*__ptr)	\
			     : "r" (__new), "0" (__old)		\
			     : "memory");			\
		break;						\
	}							\
	case 8:							\
	{							\
		volatile u64 *__ptr = (volatile u64 *)(ptr);	\
		asm volatile(lock "cmpxchgq %2,%1"		\
			     : "=a" (__ret), "+m" (*__ptr)	\
			     : "r" (__new), "0" (__old)		\
			     : "memory");			\
		break;						\
	}							\
	default:						\
		fam_atomic_compare_exchange_wrong_size();	\
	}							\
	__ret;							\
})

#define __x86_cmpxchg(ptr, old, new, size) \
	__x86_raw_cmpxchg((ptr), (old), (new), (size), LOCK_PREFIX)

#define __x86_xchg_op(ptr, arg, op, lock)				\
	({								\
		__typeof__ (*(ptr)) __ret = (arg);			\
		switch (sizeof(*(ptr))) {				\
		case 4:							\
			asm volatile (lock #op "l %0, %1\n"		\
				      : "+r" (__ret), "+m" (*(ptr))	\
				      : : "memory", "cc");		\
			break;						\
		case 8:							\
			asm volatile (lock #op "q %q0, %1\n"		\
				      : "+r" (__ret), "+m" (*(ptr))	\
				      : : "memory", "cc");		\
			break;						\
		default:						\
		       fam_atomic_ ## op ## _wrong_size();		\
		}							\
		__ret;							\
	})

#define __x86_xadd(ptr, inc, lock)	__x86_xchg_op((ptr), (inc), xadd, lock)

#define __x86_cmpxchg16(pfx, p1, p2, o1, o2, n1, n2)			\
({									\
	bool __ret;							\
	__typeof__(*(p1)) __old1 = (o1), __new1 = (n1);			\
	__typeof__(*(p2)) __old2 = (o2), __new2 = (n2);			\
	asm volatile(pfx "cmpxchg%c4b %2; sete %0"			\
		     : "=a" (__ret), "+d" (__old2),			\
		       "+m" (*(p1)), "+m" (*(p2))			\
		     : "i" (2 * sizeof(long)), "a" (__old1),		\
		       "b" (__new1), "c" (__new2));			\
	__ret;								\
})

/* #define __x86_cmpxchg16(pfx, p1, p2, o1, o2, n1, n2)			\ */
/* ({									\ */
/* 	bool __ret;							\ */
/* 	__typeof__(*(p1)) __old1 = (o1), __new1 = (n1);			\ */
/* 	__typeof__(*(p2)) __old2 = (o2), __new2 = (n2);			\ */
/* 	asm volatile(pfx "cmpxchg16b %2; sete %0"			\ */
/* 		     : "=q" (__ret), "+a" (__old1),			\ */
/* 		       "+m" (*(p1)), "+m" (*(p2)), "+d" (__old2)	\ */
/* 		     : "b" (__new1), "c" (__new2));			\ */
/* 	__ret;								\ */
/* }) */

#define xchg(ptr, v)		__x86_xchg_op((ptr), (v), xchg, "")
#define xadd(ptr, inc)		__x86_xadd((ptr), (inc), LOCK_PREFIX)
#define cmpxchg(ptr, old, new)	__x86_cmpxchg(ptr, old, new, sizeof(*(ptr)))
#define cmpxchg16(p1, p2, o1, o2, n1, n2) \
	__x86_cmpxchg16(LOCK_PREFIX, p1, p2, o1, o2, n1, n2)


static inline void ioctl_4(struct fam_atomic_args_32 *args, unsigned int opt)
{
	int32_t *atomic = (int32_t *)args->offset;
	int32_t *result_ptr = &args->p32_0;
	int32_t prev;

	switch (opt) {
	case FAM_ATOMIC_32_FETCH_AND_ADD:
		prev = xadd(atomic, args->p32_0);
		*result_ptr = prev;
		break;

	case FAM_ATOMIC_32_SWAP:
		prev = xchg(atomic, args->p32_0);
		*result_ptr = prev;
		break;

	case FAM_ATOMIC_32_COMPARE_AND_STORE:
		prev = cmpxchg(atomic, args->p32_0, args->p32_1);
		*result_ptr = prev;
		break;
	}
}

static inline void ioctl_8(struct fam_atomic_args_64 *args, unsigned int opt)
{
	int64_t *atomic = (int64_t *)args->offset;
	int64_t *result_ptr = &args->p64_0;
	int64_t prev;

	switch (opt) {
	case FAM_ATOMIC_64_FETCH_AND_ADD:
		prev = xadd(atomic, args->p64_0);
		*result_ptr = prev;
		break;

	case FAM_ATOMIC_64_SWAP:
		prev = xchg(atomic, args->p64_0);
		*result_ptr = prev;
		break;

	case FAM_ATOMIC_64_COMPARE_AND_STORE:
		prev = cmpxchg(atomic, args->p64_0, args->p64_1);
		*result_ptr = prev;
		break;
	}
}

static inline void ioctl_16(struct fam_atomic_args_128 *args, unsigned int opt)
{
	int64_t *address1 = (int64_t *)args->offset;
	int64_t *address2 = (int64_t *)((int64_t)address1 + sizeof(int64_t));
	int64_t *result1 = &(args->p128_0[0]);
	int64_t *result2 = &(args->p128_0[1]);
	bool ret;
	int64_t old[2];

	switch(opt) {
	case FAM_ATOMIC_128_SWAP:
		for (;;) {
			old[0] = xadd(address1, 0);
			old[1] = xadd(address2, 0);

			ret = cmpxchg16(address1, address2,
					old[0], old[1],
					args->p128_0[0], args->p128_0[1]);

			if (ret) {
				*result1 = old[0];
				*result2 = old[1];
				break;
			}

		}
		break;

	case FAM_ATOMIC_128_COMPARE_AND_STORE:
		for (;;) {
			ret = cmpxchg16(address1, address2,
					args->p128_0[0], args->p128_0[1],
					args->p128_1[0], args->p128_1[1]);

			if (ret) {
				*result1 = args->p128_0[0];
				*result2 = args->p128_0[1];
				break;
			} else {
				/*
				 * cmpxchg16 returned false. Sample the atomic
				 * values to obtain the "old" values, and verify
				 * they do not match the compare values so that
				 * users can correctly check that the operation
				 * did not succeed. Otherwise, retry the operation.
				 */
				old[0] = xadd(address1, 0);
				old[1] = xadd(address2, 0);

				if (old[0] != args->p128_0[0] ||
				    old[1] != args->p128_0[1]) {
					*result1 = old[0];
					*result2 = old[1];
					break;
				}
			}

		}
		break;

	case FAM_ATOMIC_128_READ:
		for (;;) {		    
		    *result1 = *address1;
		    *result2 = *address2;
		    break;
		    
			/* old[0] = xadd(address1, 0); */
			/* old[1] = xadd(address2, 0); */

			/* ret = cmpxchg16(address1, address2, */
			/* 		old[0], old[1], old[0], old[1]); */

			/* if (ret) { */
			/* 	*result1 = old[0]; */
			/* 	*result2 = old[1]; */
			/* 	break; */
			/* } */
		}
		break;
	}
}

static inline int simulated_ioctl(unsigned int opt, unsigned long args)
{
	if (opt == FAM_ATOMIC_32_FETCH_AND_ADD ||
	    opt == FAM_ATOMIC_32_SWAP ||
	    opt == FAM_ATOMIC_32_COMPARE_AND_STORE) {
		ioctl_4((struct fam_atomic_args_32 *)args, opt);
	} else if (opt == FAM_ATOMIC_64_FETCH_AND_ADD ||
		   opt == FAM_ATOMIC_64_SWAP ||
		   opt == FAM_ATOMIC_64_COMPARE_AND_STORE) {
		ioctl_8((struct fam_atomic_args_64 *)args, opt);
	} else if (opt == FAM_ATOMIC_128_SWAP ||
		   opt == FAM_ATOMIC_128_COMPARE_AND_STORE ||
		   opt == FAM_ATOMIC_128_READ) {
		ioctl_16((struct fam_atomic_args_128 *)args, opt);
	} else {
		printf("ERROR: ioctl() invalid 'opt' argument\n");
		exit(-1);
	}

	return 0;
}

static inline void __ioctl(int fd, unsigned int opt, unsigned long args)
{
    assert(0);
    return;
}

int fam_atomic_register_region(void *region_start, size_t region_length,
			       int fd, off_t offset)
{
    return 0;
}

void fam_atomic_unregister_region(void *region_start, size_t region_length)
{
    return;
}

/*
 * Returns true if we should use zbridge atomics, else returns false.
 */
static inline bool fam_atomic_get_fd_offset(void *address, int *dev_fd, int *lfs_fd, int64_t *offset)
{
    *offset = (int64_t)address;
    return false;
}

int32_t fam_atomic_32_fetch_add(int32_t *address, int32_t increment)
{
#ifdef NON_CACHE_COHERENT
	int32_t prev;
	int dev_fd, lfs_fd;
	int64_t offset;
	struct fam_atomic_args_32 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p32_0 = increment;

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_32_FETCH_AND_ADD, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_32_FETCH_AND_ADD, (unsigned long)&args);

	prev = args.p32_0;

	return prev;
#else
    return (int32_t) __atomic_fetch_add((int32_t *) address, increment, __ATOMIC_ACQ_REL);
#endif
}

int64_t fam_atomic_64_fetch_add(int64_t *address, int64_t increment)
{
#ifdef NON_CACHE_COHERENT
	int64_t prev;
	int dev_fd, lfs_fd;
	int64_t offset;
	struct fam_atomic_args_64 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p64_0 = increment;

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_64_FETCH_AND_ADD, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_64_FETCH_AND_ADD, (unsigned long)&args);

	prev = args.p64_0;

	return prev;
#else
    return (int64_t) __atomic_fetch_add((int64_t *) address, increment, __ATOMIC_ACQ_REL);
#endif
}

int32_t fam_atomic_32_swap(int32_t *address, int32_t value)
{
#ifdef NON_CACHE_COHERENT
	int32_t prev;
	int dev_fd, lfs_fd;
	int64_t offset;
	struct fam_atomic_args_32 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p32_0 = value;

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_32_SWAP, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_32_SWAP, (unsigned long)&args);

	prev = args.p32_0;

	return prev;
#else
    return (int32_t) __atomic_exchange_n((int32_t *) address, value, __ATOMIC_ACQ_REL);
#endif
}

int64_t fam_atomic_64_swap(int64_t *address, int64_t value)
{
#ifdef NON_CACHE_COHERENT
	int64_t prev;
	int dev_fd, lfs_fd;
	int64_t offset;
	struct fam_atomic_args_64 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p64_0 = value;

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_64_SWAP, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_64_SWAP, (unsigned long)&args);

	prev = args.p64_0;

	return prev;
#else
    return (int64_t) __atomic_exchange_n((int64_t *) address, value, __ATOMIC_ACQ_REL);
#endif
}

void fam_atomic_128_swap(int64_t *address, int64_t value[2], int64_t result[2])
{
	int64_t old[2];
	int dev_fd, lfs_fd;
	int64_t offset;
	bool ret;
	struct fam_atomic_args_128 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p128_0[0] = value[0];
	args.p128_0[1] = value[1];

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_128_SWAP, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_128_SWAP, (unsigned long)&args);

	result[0] = args.p128_0[0];
	result[1] = args.p128_0[1];
}

int32_t fam_atomic_32_compare_store(int32_t *address,
						 int32_t compare,
						 int32_t store)
{
#ifdef NON_CACHE_COHERENT
	int32_t prev;
	int dev_fd, lfs_fd;
	int64_t offset;
	struct fam_atomic_args_32 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p32_0 = compare;
	args.p32_1 = store;

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_32_COMPARE_AND_STORE, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_32_COMPARE_AND_STORE, (unsigned long)&args);

	prev = args.p32_0;

	return prev;
#else
    __atomic_compare_exchange_n((int32_t *) address, &compare, store, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return compare;
#endif
}

int64_t fam_atomic_64_compare_store(int64_t *address,
					  	 int64_t compare,
						 int64_t store)
{
#ifdef NON_CACHE_COHERENT
	int64_t prev;
	int dev_fd, lfs_fd;
	int64_t offset;
	struct fam_atomic_args_64 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p64_0 = compare;
	args.p64_1 = store;

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_64_COMPARE_AND_STORE, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_64_COMPARE_AND_STORE, (unsigned long)&args);

	prev = args.p64_0;

	return prev;
#else
    __atomic_compare_exchange_n((int64_t *) address, &compare, store, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return compare;
#endif
}

void fam_atomic_128_compare_store(int64_t *address,
					       int64_t compare[2],
					       int64_t store[2],
					       int64_t result[2])
{
	int64_t old[2];
	int dev_fd, lfs_fd;
	int64_t offset;
	bool ret;
	struct fam_atomic_args_128 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;
	args.p128_0[0] = compare[0];
	args.p128_0[1] = compare[1];
	args.p128_1[0] = store[0];
	args.p128_1[1] = store[1];

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_128_COMPARE_AND_STORE, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_128_COMPARE_AND_STORE, (unsigned long)&args);

	result[0] = args.p128_0[0];
	result[1] = args.p128_0[1];
}

int32_t fam_atomic_32_read(int32_t *address)
{
#ifdef NON_CACHE_COHERENT
	return fam_atomic_32_fetch_add(address, 0);
#else
    return (int32_t) __atomic_load_n((int32_t *) address, __ATOMIC_ACQUIRE);
#endif
}

int64_t fam_atomic_64_read(int64_t *address)
{
#ifdef NON_CACHE_COHERENT
	return fam_atomic_64_fetch_add(address, 0);
#else
    return (int64_t) __atomic_load_n((int64_t *) address, __ATOMIC_ACQUIRE);
#endif
}

extern void fam_atomic_128_read(int64_t *address, int64_t result[2])
{
	int64_t old[2];
	int dev_fd, lfs_fd;
	int64_t offset;
	bool ret;
	struct fam_atomic_args_128 args;
	bool use_zbridge_atomics;

	use_zbridge_atomics = fam_atomic_get_fd_offset(address, &dev_fd, &lfs_fd, &offset);

	args.lfs_fd = lfs_fd;
	args.offset = offset;

	if (use_zbridge_atomics)
		__ioctl(dev_fd, FAM_ATOMIC_128_READ, (unsigned long)&args);
	else
		simulated_ioctl(FAM_ATOMIC_128_READ, (unsigned long)&args);


	result[0] = args.p128_0[0];
	result[1] = args.p128_0[1];
}

void fam_atomic_32_write(int32_t *address, int32_t value)
{
#ifdef NON_CACHE_COHERENT
	/* This is a write operation, so no need to return prev value. */
	(void) fam_atomic_32_swap(address, value);
#else
    __atomic_store_n((int32_t *) address, (int32_t) value, __ATOMIC_RELEASE);
#endif
}

void fam_atomic_64_write(int64_t *address, int64_t value)
{
#ifdef NON_CACHE_COHERENT
	/* This is a write operation, so no need to return prev value. */
	(void) fam_atomic_64_swap(address, value);
#else
    __atomic_store_n((int64_t *) address, (int64_t) value, __ATOMIC_RELEASE);
#endif
}

void fam_atomic_128_write(int64_t *address, int64_t value[2])
{
	/*
	 * Only a write operation, so we won't need to use the 'result',
	 * but we need to create this in order to use the 128 swap API.
	 */
	int64_t result[2];

	fam_atomic_128_swap(address, value, result);
}

/*
 * TODO: For fetch_{and,or,xor}, we always guess the initial CAS 'compare'
 * value as 0. As an optimization, we can consider more advanced techniques
 * such as maintaining a cache of values stored to recently used atomics.
 * This can improve the accuracy of the guess.
 */
int32_t fam_atomic_32_fetch_and(int32_t *address, int32_t arg)
{
#ifdef NON_CACHE_COHERENT
	/*
	 * Reading the fam atomic value requires an additional system call.
	 * So we'll just guess the value as 0 for the initial CAS. If the
	 * guess is incorrect, we'll treat the CAS() as the atomic read
	 * since CAS() returns the prev value.
	 */
	int32_t prev = 0;

	for (;;) {
		int32_t actual = fam_atomic_32_compare_store(address, prev, prev & arg);

		if (actual == prev)
			return prev;

		prev = actual;
	}
#else
    return (int32_t) __atomic_fetch_and((int32_t *) address, (int32_t) arg, __ATOMIC_ACQ_REL);
#endif
}

int64_t fam_atomic_64_fetch_and(int64_t *address, int64_t arg)
{
#ifdef NON_CACHE_COHERENT
	int64_t prev = 0;

	for (;;) {
		int64_t actual = fam_atomic_64_compare_store(address, prev, prev & arg);

		if (actual == prev)
			return prev;

		prev = actual;
	}
#else
    return (int64_t) __atomic_fetch_and((int64_t *) address, (int64_t) arg, __ATOMIC_ACQ_REL);
#endif
}

int32_t fam_atomic_32_fetch_or(int32_t *address, int32_t arg)
{
#ifdef NON_CACHE_COHERENT
	int32_t prev = 0;

	for (;;) {
		int32_t actual = fam_atomic_32_compare_store(address, prev, prev | arg);

		if (actual == prev)
			return prev;

		prev = actual;
	}
#else
    return (int32_t) __atomic_fetch_or((int32_t *) address, (int32_t) arg, __ATOMIC_ACQ_REL);
#endif
}

int64_t fam_atomic_64_fetch_or(int64_t *address, int64_t arg)
{
#ifdef NON_CACHE_COHERENT
	int64_t prev = 0;

	for (;;) {
		int64_t actual = fam_atomic_64_compare_store(address, prev, prev | arg);

		if (actual == prev)
			return prev;

		prev = actual;
	}
#else
    return (int64_t) __atomic_fetch_or((int64_t *) address, (int64_t) arg, __ATOMIC_ACQ_REL);
#endif
}

int32_t fam_atomic_32_fetch_xor(int32_t *address, int32_t arg)
{
#ifdef NON_CACHE_COHERENT
	int32_t prev = 0;

	for (;;) {
		int32_t actual = fam_atomic_32_compare_store(address, prev, prev ^ arg);

		if (actual == prev)
			return prev;

		prev = actual;
	}
#else
    return (int32_t) __atomic_fetch_xor((int32_t *) address, (int32_t) arg, __ATOMIC_ACQ_REL);
#endif
}

int64_t fam_atomic_64_fetch_xor(int64_t *address, int64_t arg)
{
#ifdef NON_CACHE_COHERENT
	int64_t prev = 0;

	for (;;) {
		int64_t actual = fam_atomic_64_compare_store(address, prev, prev ^ arg);

		if (actual == prev)
			return prev;

		prev = actual;
	}
#else
    return (int64_t) __atomic_fetch_xor((int64_t *) address, (int64_t) arg, __ATOMIC_ACQ_REL);
#endif
}

void fam_spin_lock_init(struct fam_spinlock *lock)
{
        fam_atomic_64_write(&lock->head_tail, 0);
}

void fam_spin_lock(struct fam_spinlock *lock)
{
        struct fam_spinlock inc = {
                .tickets = {
                        .head = 0,
                        .tail = 1
                }
        };

        /* Fetch the current values and bump the tail by one */
#ifdef NON_CACHE_COHERENT
        inc.head_tail = fam_atomic_64_fetch_add(&lock->head_tail, inc.head_tail);
#else
        inc.head_tail = __atomic_fetch_add(&lock->head_tail, inc.head_tail, __ATOMIC_ACQ_REL);
#endif

        if (inc.tickets.head != inc.tickets.tail) {
                for (;;) {
#ifdef NON_CACHE_COHERENT
                        inc.tickets.head = fam_atomic_32_fetch_add(&lock->tickets.head, 0);
#else
                        inc.tickets.head = __atomic_fetch_add(&lock->tickets.head, 0, __ATOMIC_ACQ_REL);
#endif
                        if (inc.tickets.head == inc.tickets.tail)
                                break;
                }
        }
#ifdef NON_CACHE_COHERENT
        __sync_synchronize();
#endif
}

bool fam_spin_trylock(struct fam_spinlock *lock)
{
        struct fam_spinlock old, new;
        bool ret;

#ifdef NON_CACHE_COHERENT
        old.head_tail = fam_atomic_64_fetch_add(&lock->head_tail, (int64_t) 0);
#else
        old.head_tail = __atomic_fetch_add(&lock->head_tail, (int64_t) 0, __ATOMIC_ACQ_REL);
#endif
        if (old.tickets.head != old.tickets.tail)
                return 0;

        new.tickets.head = old.tickets.head;
        new.tickets.tail = old.tickets.tail + 1;
#ifdef NON_CACHE_COHERENT
        ret = fam_atomic_64_compare_store(&lock->head_tail, old.head_tail, new.head_tail) == old.head_tail;
        __sync_synchronize();
#else
        ret = __atomic_compare_exchange_n(&lock->head_tail, &old.head_tail, 
                new.head_tail, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
        return ret;
}

void fam_spin_unlock(struct fam_spinlock *lock)
{
#ifdef NON_CACHE_COHERENT
        (void) fam_atomic_32_fetch_add(&lock->tickets.head, 1);
        __sync_synchronize();
#else
        __atomic_fetch_add(&lock->tickets.head, (int32_t) 1, __ATOMIC_ACQ_REL);
#endif
}
