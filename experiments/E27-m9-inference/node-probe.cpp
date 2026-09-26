#include "arg.h"
#include "common.h"
#include "debug.h"
#include "log.h"
#include "llama.h"

#include <clocale>
#include <string>
#include <vector>


// E27 diagnostic, based on upstream eval-callback (MIT). Read inputs separately
// after scheduler synchronization, preserving exact F32 data for comparison.
#include "ggml-backend.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
static unsigned probe_node = 0;
static bool probe_failed = false;
static void probe_tensor(const char * role, ggml_tensor * t) {
    if (!t || t->type != GGML_TYPE_F32 || !ggml_is_contiguous(t)) return;
    const size_t n = ggml_nelements(t);
    std::vector<float> v(n);
    ggml_backend_tensor_get(t, v.data(), 0, n * sizeof(float));
    double sum = 0;
    for (float x : v) sum += x;
    fprintf(stdout, "PROBE %u %s %s ne=%lld,%lld,%lld,%lld data=%p buffer=%p sum=%.12g first=",
            probe_node, role, t->name, (long long)t->ne[0], (long long)t->ne[1],
            (long long)t->ne[2], (long long)t->ne[3], t->data, (void*)t->buffer, sum);
    for (size_t i=0; i<8 && i<n; ++i) fprintf(stdout," %.9g",v[i]);
    fprintf(stdout,"\n");
    const char * dir = getenv("BC250_TENSOR_DIR");
    if (dir) {
        char path[1024]; snprintf(path,sizeof(path),"%s/%03u-%s.f32",dir,probe_node,role);
        FILE * f=fopen(path,"wb");
        if (!f || fwrite(v.data(),sizeof(float),n,f)!=n) { fprintf(stderr,"tensor dump failed\n"); exit(2); }
        fclose(f);
    }
}
static bool probe_eval(ggml_tensor * t, bool ask, void *) {
    if (ask) return probe_node < 256;
    fprintf(stdout,"NODE %u %s %s\n",probe_node,t->name,ggml_op_name(t->op));
    probe_tensor("src0",t->src[0]);
    probe_tensor("src1",t->src[1]);
    probe_tensor("dst",t);
    fflush(stdout);
    const char * logdir = getenv("BC250_KMD_LOG_DIR");
    if (logdir) {
        char cmd[2048];
        snprintf(cmd,sizeof(cmd),"C:\\BC250\\m8\\bc250kmd_cli.exe log summary > %s\\%03u-kmd.txt",logdir,probe_node);
        if (system(cmd) != 0) { probe_failed = true; fprintf(stderr,"KMD_LOG_FAILED node=%u\n",probe_node); return false; }
        char path[1024]; snprintf(path,sizeof(path),"%s/%03u-kmd.txt",logdir,probe_node);
        FILE * f=fopen(path,"r");
        if (!f) { probe_failed = true; return false; }
        char line[1024]; bool fault=false;
        while (fgets(line,sizeof(line),f)) {
            const char * marker=strstr(line,"node 0 hardware:");
            unsigned long submitted=0,completed=0,timeouts=0,refused=0;
            if (marker && sscanf(marker,"node 0 hardware: %lu submitted, %lu completed, %lu timeouts, %lu refused",&submitted,&completed,&timeouts,&refused)==4 && (timeouts || refused)) fault=true;
        }
        fclose(f);
        if (fault) { probe_failed = true; fprintf(stdout,"STOP_ON_HARDWARE_FAILURE node=%u\n",probe_node); fflush(stdout); return false; }
    }
    ++probe_node;
    return true;
}

static bool run(llama_context * ctx, const common_params & params) {
    const llama_model * model = llama_get_model(ctx);
    const llama_vocab * vocab = llama_model_get_vocab(model);

    const bool add_bos = llama_vocab_get_add_bos(vocab);

    std::vector<llama_token> tokens = common_tokenize(ctx, params.prompt, add_bos, true);

    if (tokens.empty()) {
        LOG_ERR("%s : there are not input tokens to process - (try to provide a prompt with '-p')\n", __func__);
        return false;
    }

    LOG_INF("number of input tokens = %zu\n", tokens.size());
    for (size_t i = 0; i < tokens.size(); ++i) {
        LOG_INF("  %d\n", tokens[i]);
    }

    if (llama_decode(ctx, llama_batch_get_one(tokens.data(), tokens.size()))) {
        LOG_ERR("%s : failed to eval\n", __func__);
        return false;
    }

    return true;
}

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");

    common_debug_cb_user_data cb_data;

    common_params params;

    common_init();

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_COMMON)) {
        return 1;
    }

    llama_backend_init();
    llama_numa_init(params.numa);

    // pass the callback to the backend scheduler
    // it will be executed for each node during the graph computation
    params.cb_eval = probe_eval;
    params.cb_eval_user_data = &cb_data;
    params.warmup = false;

    // init
    auto llama_init = common_init_from_params(params);

    auto * model = llama_init->model();
    auto * ctx   = llama_init->context();

    if (model == nullptr || ctx == nullptr) {
        LOG_ERR("%s : failed to init\n", __func__);
        return 1;
    }

    // print system information
    {
        LOG_INF("\n");
        LOG_INF("%s\n", common_params_get_system_info(params).c_str());
        LOG_INF("\n");
    }

    bool OK = run(ctx, params) && !probe_failed;
    if (!OK) {
        return 1;
    }

    LOG("\n");
    llama_perf_context_print(ctx);

    llama_backend_free();

    return 0;
}
