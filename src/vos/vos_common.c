/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * Common internal functions for VOS
 * vos/vos_common.c
 *
 * Author: Vishwanath Venkatesan <vishwanath.venkatesan@intel.com>
 */
#define DDSUBSYS	DDFAC(vos)

#include <daos/common.h>
#include <daos/rpc.h>
#include <daos_srv/daos_server.h>
#include <vos_internal.h>
#include <daos/lru.h>

static pthread_mutex_t	mutex = PTHREAD_MUTEX_INITIALIZER;
/**
 * Object cache based on mode of instantiation
 */
struct daos_lru_cache*
vos_get_obj_cache(void)
{
#ifdef VOS_STANDALONE
	return vsa_imems_inst->vis_ocache;
#else
	return vos_tls_get()->vtl_imems_inst.vis_ocache;
#endif
}

int
vos_csum_enabled(void)
{
#ifdef VOS_STANDALONE
	return vsa_imems_inst->vis_enable_checksum;
#else
	return vos_tls_get()->vtl_imems_inst.vis_enable_checksum;
#endif
}

int
vos_csum_compute(daos_sg_list_t *sgl, daos_csum_buf_t *csum)
{
	int	i;
	int	rc;
#ifdef VOS_STANDALONE
	mchecksum_object_t *checksum = &vsa_imems_inst->vis_checksum;
#else
	mchecksum_object_t *checksum =
		&vos_tls_get()->vtl_imems_inst.vis_checksum;
#endif

	if (!sgl->sg_iovs)
		return 0;

	rc = mchecksum_reset(*checksum);
	if (rc <= 0) {
		D__ERROR("Error in resetting checksum: %d\n", rc);
		D__GOTO(failed, rc = -DER_IO);
	}

	for (i = 0; i < sgl->sg_nr_out; i++) {
		if (!sgl->sg_iovs[i].iov_buf ||
		    !sgl->sg_iovs[i].iov_len)
			continue;

		/* accumulates a partial checksum of the input data */
		rc = mchecksum_update(*checksum, sgl->sg_iovs[i].iov_buf,
				      sgl->sg_iovs[i].iov_len);
		if (rc <= 0) {
			D__ERROR("Error in updating checksum: %d\n", rc);
			D__GOTO(failed, rc = -DER_IO);
		}
	}
	rc = 0;
	csum->cs_len = mchecksum_get_size(*checksum);
	D__ASSERT(csum->cs_buf_len >= csum->cs_len);

	/* get checksum result */
	mchecksum_get(*checksum, csum->cs_csum, csum->cs_buf_len,
		      MCHECKSUM_FINALIZE /* Unused for crc64 */);
failed:
	return rc;
}
/**
 * VOS in-memory structure creation.
 * Handle-hash:
 * -----------
 * Uses in-memory daos_uuid hash to maintain one
 * reference per thread in heap for each pool/container.
 * Calls to pool/container open/close track references
 * through internal refcounting.
 *
 * Object-cache:
 * ------------
 * In-memory object cache for object index in PMEM
 * Created once for standalone mode and once for every
 * TLS instance.
 */

static inline void
vos_imem_strts_destroy(struct vos_imem_strts *imem_inst)
{
	if (imem_inst->vis_ocache)
		vos_obj_cache_destroy(imem_inst->vis_ocache);

	if (imem_inst->vis_pool_hhash)
		d_uhash_destroy(imem_inst->vis_pool_hhash);

	if (imem_inst->vis_cont_hhash)
		d_uhash_destroy(imem_inst->vis_cont_hhash);
}

static inline int
vos_imem_strts_create(struct vos_imem_strts *imem_inst)
{
	char *env;
	int   rc;

	rc = vos_obj_cache_create(LRU_CACHE_BITS,
				  &imem_inst->vis_ocache);
	if (rc) {
		D__ERROR("Error in createing object cache\n");
		return rc;
	}

	rc = d_uhash_create(0 /* no locking */, VOS_POOL_HHASH_BITS,
			    &imem_inst->vis_pool_hhash);
	if (rc) {
		D__ERROR("Error in creating POOL ref hash: %d\n", rc);
		goto failed;
	}

	rc = d_uhash_create(0 /* no locking */, VOS_CONT_HHASH_BITS,
			    &imem_inst->vis_cont_hhash);
	if (rc) {
		D__ERROR("Error in creating CONT ref hash: %d\n", rc);
		goto failed;
	}

	env = getenv("VOS_CHECKSUM");
	if (daos_csum_supported(env)) {
		rc = mchecksum_init(env, &imem_inst->vis_checksum);
		if (!rc) {
			D__ERROR("Error in initializing checksum\n");
			goto failed;
		}

		D__DEBUG(DB_IO, "Enable VOS checksum=%s\n", env);
		imem_inst->vis_enable_checksum = 1;
	} else {
		imem_inst->vis_enable_checksum = 0;
	}

	return 0;
failed:
	vos_imem_strts_destroy(imem_inst);
	return rc;
}

static void *
vos_tls_init(const struct dss_thread_local_storage *dtls,
	     struct dss_module_key *key)
{
	struct vos_tls *tls;

	D__ALLOC_PTR(tls);
	if (tls == NULL)
		return NULL;

	if (vos_imem_strts_create(&tls->vtl_imems_inst)) {
		D__FREE_PTR(tls);
		return NULL;
	}

	return tls;
}

static void
vos_tls_fini(const struct dss_thread_local_storage *dtls,
	     struct dss_module_key *key, void *data)
{
	struct vos_tls *tls = data;

	vos_imem_strts_destroy(&tls->vtl_imems_inst);
	D__FREE_PTR(tls);
}

struct dss_module_key vos_module_key = {
	.dmk_tags = DAOS_SERVER_TAG,
	.dmk_index = -1,
	.dmk_init = vos_tls_init,
	.dmk_fini = vos_tls_fini,
};

static int
vos_mod_init(void)
{
	char	*env;
	int	 rc = 0;

	/* This is for performance evaluation only, all data will be stored
	 * in DRAM by setting this.
	 */
	env = getenv("VOS_MEM_CLASS");
	if (env && strcasecmp(env, "DRAM") == 0) {
		D__WARN("Running in DRAM mode, all data are volatile.\n");
		vos_mem_class = UMEM_CLASS_VMEM;
	}

	rc = vos_cont_tab_register();
	if (rc) {
		D__ERROR("VOS CI btree initialization error\n");
		return rc;
	}

	/**
	 * Registering the class for OI btree
	 * and KV btree
	 */
	rc = vos_obj_tab_register();
	if (rc) {
		D__ERROR("VOS OI btree initialization error\n");
		return rc;
	}

	rc = vos_cookie_tab_register();
	if (rc) {
		D__ERROR("VOS cookie btree initialization error\n");
		return rc;
	}

	rc = vos_obj_tree_register();
	if (rc)
		D__ERROR("Failed to register vos trees\n");

	return rc;
}

static int
vos_mod_fini(void)
{
	return 0;
}

struct dss_module vos_srv_module =  {
	.sm_name	= "vos_srv",
	.sm_mod_id	= DAOS_VOS_MODULE,
	.sm_ver		= 1,
	.sm_init	= vos_mod_init,
	.sm_fini	= vos_mod_fini,
	.sm_key		= &vos_module_key,
};

int
vos_init(void)
{
	int		rc = 0;
	static int	is_init = 0;

	if (is_init) {
		D__ERROR("Already initialized a VOS instance\n");
		return rc;
	}

	pthread_mutex_lock(&mutex);

	if (is_init && vsa_imems_inst)
		D__GOTO(exit, rc);

	D__ALLOC_PTR(vsa_imems_inst);
	if (vsa_imems_inst == NULL)
		D__GOTO(exit, rc);

	rc = vos_imem_strts_create(vsa_imems_inst);
	if (rc)
		D__GOTO(exit, rc);

	rc = vos_mod_init();
	if (rc)
		D__GOTO(exit, rc);

	is_init = 1;
exit:
	pthread_mutex_unlock(&mutex);
	if (rc && vsa_imems_inst)
		D__FREE_PTR(vsa_imems_inst);
	return rc;
}

void
vos_fini(void)
{
	pthread_mutex_lock(&mutex);
	if (vsa_imems_inst) {
		vos_imem_strts_destroy(vsa_imems_inst);
		D__FREE_PTR(vsa_imems_inst);
	}
	pthread_mutex_unlock(&mutex);
}
