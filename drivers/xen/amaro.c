#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/interface.h>
#include <asm/xen/page.h>
#include <asm/pgtable_types.h>
#include <linux/memcontrol.h>

#include <xen/amaro.h>

#define MAX_HOT_MFN 32000
#define NUM_SHARED_PAGES 32

void *vaddrs[NUM_SHARED_PAGES];
unsigned long mfns[NUM_SHARED_PAGES];
xen_pfn_t frame_list[MAX_HOT_MFN];
int hsm_init_flag=0;
static int alloc_shared_pages(void);
static void free_shared_pages(void);

/*perf counter related declarations*/
#define READ_PERF_CNTRS
#define MAX_VCPUS 12
#define RESET_PERFCOUNT 0
#define READ_PERFCOUNT 1

struct perf_ctrs *perfcntr;
struct perf_ctrs p_cores_sum;
struct perf_ctrs prev_cores_sum;
spinlock_t perfrd_lock;

asmlinkage long sys_hsm_read(unsigned int start)
{
    unsigned int max_frames, frames_ppage, pidx;
    long fidx;
    unsigned long offset;
    struct frame *f;
    void *curr_base_vaddr;

    frames_ppage = PAGE_SIZE / sizeof(struct frame);
    max_frames = NUM_SHARED_PAGES * frames_ppage;
    offset = 0;
    start = start % max_frames;
    fidx = 0;
  
    //printk("start=%u, frames_ppage=%u\n", start, frames_ppage);

    for(pidx = (start * NUM_SHARED_PAGES) / max_frames; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        printk("pidx=%u\n", pidx);
        curr_base_vaddr = vaddrs[pidx];

        for(fidx = start - (frames_ppage * pidx); fidx < frames_ppage; ++fidx)
        {
            offset = fidx * sizeof(struct frame);
            f = (void *)(((unsigned long)curr_base_vaddr) + offset);

            if (f->mfn == 0)
            {
                //printk("pidx=%u fidx=%ld mfn=0 break\n", pidx, fidx);
                return fidx + (frames_ppage * pidx);
            }
            else
            {
                //printk("pidx=%u fidx=%ld mfn=%u\n", pidx, fidx, f->mfn);
            }
        }
    }

    return fidx + (frames_ppage * pidx);
}

static int alloc_shared_pages(void)
{
    long ret;
    unsigned int pidx;

    for (pidx = 0; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        printk("allocating pidx=%u\n", pidx);
        if ((ret = HYPERVISOR_hsm_get_mfn(&mfns[pidx])) < 0)
        {
            printk("ERROR, hsm_get_mfn() returned: %ld\n", ret);
            return 1;
        }

        vaddrs[pidx] = kmalloc(PAGE_SIZE, GFP_KERNEL);
        set_pte_vaddr((unsigned long) vaddrs[pidx],
                      mfn_pte(mfns[pidx], PAGE_KERNEL));
    }
    
    //printk("allocation ended\n");

    return 0;
}

int reset_perf_counters(void)
{
	int ret, count;
	static struct perf_ctrs tmpperfcntr;
	//GUEST_HANDLE(perf_ctrs) xen_guest_handle_perfctr;
	//set_xen_guest_handle(xen_guest_handle_perfctr, &tmpperfcntr);

	printk("Tot INSTS: %lu Tot CYCLES: %lu Tot LLCMISSES: %lu \n",
	      p_cores_sum.instns, p_cores_sum.cycles, p_cores_sum.lmisses);

	prev_cores_sum.instns = p_cores_sum.instns;
	prev_cores_sum.cycles = p_cores_sum.cycles;
	prev_cores_sum.lmisses = p_cores_sum.lmisses;


	p_cores_sum.instns = 0;
	p_cores_sum.cycles = 0;
	p_cores_sum.lmisses = 0;

    if ((ret = HYPERVISOR_perfctr_op(RESET_PERFCOUNT, &tmpperfcntr)) < 0)
    {
      	printk("ERROR, perf_counters() returned: %ld\n", ret);
    }
	return ret;
}
EXPORT_SYMBOL(reset_perf_counters);


int get_perf_counters(void)
{
    long ret;
	int count=0;
	struct perf_ctrs p_core;
	GUEST_HANDLE(perf_ctrs) xen_guest_handle_perfctr;

	if(!perfcntr) {
		perfcntr = kzalloc(sizeof(struct perf_ctrs) * MAX_VCPUS, GFP_KERNEL);
		if(!perfcntr){
			printk(KERN_ALERT "perfcntr alloc failed \n");
			return -1;
		}
	}
	set_xen_guest_handle(xen_guest_handle_perfctr, perfcntr);

	/*Clear everything*/
	for(count=0; count < MAX_VCPUS; count++){
		perfcntr[count].instns = 0;
		perfcntr[count].cycles = 0;
		perfcntr[count].lmisses = 0;
	}

    if ((ret = HYPERVISOR_perfctr_op(READ_PERFCOUNT, xen_guest_handle_perfctr)) < 0)
    {
      	printk("ERROR, perf_counters() returned: %ld\n", ret);
        return 1;
    }

	for(count=1; count < MAX_VCPUS; count++){

		p_core = perfcntr[count];
		/*idle core or unused or offline core*/
		if(p_core.cycles == 0)
			continue;

		//printk("Core:%d INSTS: %lu CYCLES: %lu LLCMISSES: %lu \n",
		  //  count, p_core.instns, p_core.cycles, p_core.lmisses);
		p_cores_sum.instns += p_core.instns;
		p_cores_sum.cycles += p_core.cycles;
		p_cores_sum.lmisses += p_core.lmisses;
	}
	
    return 0;
}
EXPORT_SYMBOL(get_perf_counters);


int print_perf_counters(void)
{
	//get_perf_counters();
	printk("Tot INSTS: %lu Tot CYCLES: %lu Tot LLCMISSES: %lu \n",
	      p_cores_sum.instns, p_cores_sum.cycles, p_cores_sum.lmisses);

	return 0;
}
EXPORT_SYMBOL(print_perf_counters);

int MigrationEnable(){

	unsigned long diff;

	if(p_cores_sum.lmisses < 500000){
		return 0;
	}

	if(p_cores_sum.lmisses > prev_cores_sum.lmisses){
		diff = p_cores_sum.lmisses - prev_cores_sum.lmisses;
		return 1;
	}else{
		diff = prev_cores_sum.lmisses - p_cores_sum.lmisses;
		if(diff/prev_cores_sum.lmisses*100 > 50)
		/*Dont migrate*/	
		return 0;	
	}
}


asmlinkage long sys_hsm_alloc(void)
{
    printk("sys_hsm_alloc\n");
    return alloc_shared_pages();
}

static void free_shared_pages(void)
{
    int rc;
    unsigned int pidx;

    for (pidx = 0; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        kfree(vaddrs[pidx]);
    }
    
    rc = HYPERVISOR_hsm_free_mfn((uint64_t) 0); // number is ignored

    if (rc != 0) {
        printk("ERROR, hsm_free_page() returned: %d\n", rc);
    }
}

asmlinkage long sys_hsm_free(void)
{
    printk("sys_hsm_free()");
    free_shared_pages();
    return 0;
}

xen_pfn_t *get_hotpage_list_sharedmem(unsigned int *hotcnt)
{
    unsigned int frames_ppage, pidx, fidx;
    unsigned long offset;
    struct frame *f;
    void *curr_base_vaddr;

	//printk(KERN_ALERT "hsm_init_flag %u \n",hsm_init_flag);
	//spin_lock(&perfrd_lock);

    //unsigned long *lock = (unsigned long *)vaddrs[0];
	/* if not intialized, then initialize now*/
	if(hsm_init_flag == 0) {
		hsm_init_flag = 1;
		sys_hsm_alloc();
	}

#ifdef READ_PERF_CNTRS
	get_perf_counters();

	if(!MigrationEnable()){
		*hotcnt =0;
		return frame_list;
	}
	reset_perf_counters();
#endif

    frames_ppage = PAGE_SIZE / sizeof(struct frame);
    offset = 0;

    //bit_spin_lock(0, lock);
  
	*hotcnt = 0;

    for(pidx = 0; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        curr_base_vaddr = vaddrs[pidx];

        for(fidx = 0; fidx < frames_ppage; ++fidx)
        {
            //if (unlikely(pidx == 0 && fidx == 0))
              //  continue;

			if(MAX_HOT_MFN < *hotcnt)
				return frame_list;

            offset = fidx * sizeof(struct frame);
            f = (void *)(((unsigned long)curr_base_vaddr) + offset);

            if (f->mfn == 0) {
                //*hotcnt = pidx * frames_ppage + fidx;
               	//printk("hotcnt = %u fidx=%u pidx=%u "
				//		"frames_ppage=%u\n", *hotcnt, fidx, pidx, frames_ppage);
                return frame_list;
            }
			frame_list[pidx * frames_ppage + fidx] = f->mfn;
			*hotcnt = *hotcnt +1;
        }
    }

	if(MAX_HOT_MFN < *hotcnt)
	    *hotcnt = MAX_HOT_MFN;

	//spin_unlock(&perfrd_lock);

    //bit_spin_unlock(0, lock);
    return frame_list;
}
