// cuVS CAGRA 构建/搜索基准 —— 参数扫描用（AMD hipVS / gfx1100）
// 用法: ./cagra_bench N D GD IGD NITER ALGO NQ K PQ_BITS PQ_DIM NLISTS NPROBES REFINE_PCT ITOPK DTYPE BASEF QRYF OUTF
//   ALGO : 0=AUTO 1=IVF_PQ 2=NN_DESCENT 3=ITERATIVE_CAGRA_SEARCH
//   DTYPE: 32=fp32 构图（默认）, 16=fp16 构图
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/common.h>
#include <hip/hip_fp16.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static float* read_f32(const char* path, size_t n) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "open fail %s\n", path); exit(2); }
  float* buf = (float*)malloc(n * sizeof(float));
  size_t got = fread(buf, sizeof(float), n, f);
  fclose(f);
  if (got != n) { fprintf(stderr, "short read %s: %zu/%zu\n", path, got, n); exit(2); }
  return buf;
}

// 上传数据到显存: bits=32 → 直接拷贝; bits=16 → 转半精度
static void* upload(const float* h, size_t n, int bits) {
  void* d = NULL;
  if (bits == 16) {
    __half* tmp = (__half*)malloc(n * sizeof(__half));
    for (size_t i = 0; i < n; i++) tmp[i] = __float2half_rn(h[i]);
    if (cudaMalloc(&d, n * 2) != 0) { fprintf(stderr, "hipMalloc16 fail\n"); exit(3); }
    cudaMemcpy(d, tmp, n * 2, cudaMemcpyHostToDevice);
    free(tmp);
  } else {
    if (cudaMalloc(&d, n * 4) != 0) { fprintf(stderr, "hipMalloc32 fail\n"); exit(3); }
    cudaMemcpy(d, h, n * 4, cudaMemcpyHostToDevice);
  }
  return d;
}

static DLManagedTensor* make_tensor(void* dev, int64_t rows, int64_t cols, int64_t* shape_buf, int bits, int is_int) {
  DLManagedTensor* t = (DLManagedTensor*)calloc(1, sizeof(DLManagedTensor));
  t->dl_tensor.data = dev;
  t->dl_tensor.device.device_type = kDLCUDA;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = is_int ? kDLUInt : kDLFloat;
  t->dl_tensor.dtype.bits = bits;
  t->dl_tensor.dtype.lanes = 1;
  shape_buf[0] = rows; shape_buf[1] = cols;
  t->dl_tensor.shape = shape_buf;
  t->dl_tensor.strides = NULL;
  t->dl_tensor.byte_offset = 0;
  return t;
}

int main(int argc, char** argv) {
  if (argc < 18) {
    fprintf(stderr, "usage: %s N D GD IGD NITER ALGO NQ K PQ_BITS PQ_DIM NLISTS NPROBES REFINE_PCT ITOPK DTYPE BASEF QRYF OUTF\n", argv[0]);
    return 1;
  }
  int64_t N = atoll(argv[1]), D = atoll(argv[2]);
  int64_t GD = atoll(argv[3]), IGD = atoll(argv[4]), NITER = atoll(argv[5]);
  int ALGO = atoi(argv[6]);
  int64_t NQ = atoll(argv[7]), K = atoll(argv[8]);
  int PQ_BITS = atoi(argv[9]), PQ_DIM = atoi(argv[10]), NLISTS = atoi(argv[11]);
  int NPROBES = atoi(argv[12]), REFINE_PCT = atoi(argv[13]), ITOPK = atoi(argv[14]);
  int DTYPE = atoi(argv[15]);
  const char* basef = argv[16]; const char* qryf = argv[17]; const char* outf = argv[18];

  float* h_base = read_f32(basef, (size_t)N * D);
  float* h_qry = read_f32(qryf, (size_t)NQ * D);

  void* d_base = upload(h_base, (size_t)N * D, DTYPE);
  void* d_qry = upload(h_qry, (size_t)NQ * D, DTYPE);
  free(h_base); free(h_qry);

  int64_t shape_base[2], shape_qry[2], shape_nb[2], shape_ds[2];
  DLManagedTensor* t_base = make_tensor(d_base, N, D, shape_base, DTYPE, 0);
  DLManagedTensor* t_qry = make_tensor(d_qry, NQ, D, shape_qry, DTYPE, 0);

  void* d_nb = NULL; void* d_ds = NULL;
  cudaMalloc(&d_nb, (size_t)NQ * K * 4);
  cudaMalloc(&d_ds, (size_t)NQ * K * 4);
  DLManagedTensor* t_nb = make_tensor(d_nb, NQ, K, shape_nb, 32, 1);
  DLManagedTensor* t_ds = make_tensor(d_ds, NQ, K, shape_ds, 32, 0);

  cuvsResources_t res;
  if (cuvsResourcesCreate(&res) != CUVS_SUCCESS) { fprintf(stderr, "res fail\n"); return 4; }

  cuvsCagraIndexParams_t params; cuvsCagraIndexParamsCreate(&params);
  params->graph_degree = GD;
  params->intermediate_graph_degree = IGD;
  params->nn_descent_niter = NITER;
  params->build_algo = (enum cuvsCagraGraphBuildAlgo)ALGO;

  cuvsCagraCompressionParams_t comp = NULL;
  if (PQ_BITS > 0 && cuvsCagraCompressionParamsCreate(&comp) == CUVS_SUCCESS) {
    comp->pq_bits = PQ_BITS;
    comp->pq_dim = PQ_DIM;
    params->compression = comp;
  }

  struct cuvsIvfPqParams ivfpq_store;
  memset(&ivfpq_store, 0, sizeof(ivfpq_store));
  if (NLISTS > 0 || NPROBES > 0 || REFINE_PCT > 0) {
    cuvsIvfPqIndexParams_t bp;
    if (cuvsIvfPqIndexParamsCreate(&bp) == CUVS_SUCCESS) {
      if (NLISTS > 0) bp->n_lists = NLISTS;
      ivfpq_store.ivf_pq_build_params = bp;
      if (NPROBES > 0) {
        cuvsIvfPqSearchParams_t sqp;
        if (cuvsIvfPqSearchParamsCreate(&sqp) == CUVS_SUCCESS) { sqp->n_probes = NPROBES; ivfpq_store.ivf_pq_search_params = sqp; }
      }
      ivfpq_store.refinement_rate = REFINE_PCT / 100.0f;
      params->graph_build_params = &ivfpq_store;
    }
  }

  cuvsCagraIndex_t index; cuvsCagraIndexCreate(&index);
  cudaDeviceSynchronize();
  double t0 = now_ms();
  cuvsError_t st = cuvsCagraBuild(res, params, t_base, index);
  cudaDeviceSynchronize();
  double build_ms = now_ms() - t0;
  if (st != CUVS_SUCCESS) {
    printf("BUILD_FAIL algo=%d gd=%lld igd=%lld niter=%lld dtype=%d\n", ALGO, (long long)GD, (long long)IGD, (long long)NITER, DTYPE);
    return 5;
  }

  int64_t got_gd = -1; cuvsCagraIndexGetGraphDegree(index, &got_gd);

  cuvsCagraSearchParams_t sp; cuvsCagraSearchParamsCreate(&sp);
  size_t itopk = ITOPK > 0 ? (size_t)ITOPK : (K > 64 ? (size_t)K : 64);
  if (itopk < (size_t)K) itopk = (size_t)K;
  sp->itopk_size = itopk;

  cuvsFilter none; memset(&none, 0, sizeof(none)); none.type = NO_FILTER; none.addr = 0;
  double q0 = now_ms();
  cuvsError_t qst = cuvsCagraSearch(res, sp, index, t_qry, t_nb, t_ds, none);
  cudaDeviceSynchronize();
  double search_ms = now_ms() - q0;

  if (qst == CUVS_SUCCESS) {
    uint32_t* h_nb = (uint32_t*)malloc((size_t)NQ * K * 4);
    cudaMemcpy(h_nb, d_nb, (size_t)NQ * K * 4, cudaMemcpyDeviceToHost);
    FILE* fo = fopen(outf, "wb");
    if (fo) { fwrite(h_nb, 4, (size_t)NQ * K, fo); fclose(fo); }
    free(h_nb);
  }

  printf("algo=%d gd=%lld igd=%lld niter=%lld dtype=%d pq=%d/%d nl=%d np=%d rf=%.2f itopk=%d N=%lld D=%lld build_ms=%.1f search_ms=%.1f gd_actual=%lld recall_ids=%s\n",
         ALGO, (long long)GD, (long long)IGD, (long long)NITER, DTYPE, PQ_BITS, PQ_DIM, NLISTS, NPROBES,
         REFINE_PCT / 100.0, (int)itopk, (long long)N, (long long)D, build_ms, search_ms,
         (long long)got_gd, qst == CUVS_SUCCESS ? "ok" : "fail");
  return 0;
}
