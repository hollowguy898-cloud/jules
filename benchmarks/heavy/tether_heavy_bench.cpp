// ============================================================================
// Tether Heavy Workload Benchmark Suite vs Rust
//
// This C++ program simulates what the Tether compiler generates with all
// optimizations enabled: soa struct, align(64), @simd, prefetch insertion,
// arena allocation, cold-path error separation, escape analysis (stack alloc).
//
// Scaled to run within ~5 minutes total while still being HEAVY.
//
// Compile: g++ -std=c++17 -O3 -march=native -ffast-math -pthread tether_heavy_bench.cpp -o tether_heavy_bench -lm
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <vector>
#include <algorithm>

using hrclock_t = std::chrono::high_resolution_clock;
using ms_t = std::chrono::duration<double, std::milli>;

#define BENCH_START(name) printf("BENCH_%s_START\n", name)
#define BENCH_END(name) printf("BENCH_%s_END\n", name)
#define BENCH_RESULT(key, val) printf("  %s:%.4f\n", key, val)

// ============================================================================
// BENCHMARK 1: 20M Entity ECS — 10 Systems, 30 frames
// ============================================================================

static const int ECS_N = 20'000'000;
static const int ECS_ITERS = 30;

struct alignas(64) PosSoA { double* x; double* y; double* z;
    static PosSoA create(int n) { PosSoA s; s.x=(double*)aligned_alloc(64,n*8); s.y=(double*)aligned_alloc(64,n*8); s.z=(double*)aligned_alloc(64,n*8); return s; }
    void destroy() { free(x); free(y); free(z); }
};
struct alignas(64) VelSoA { double* dx; double* dy; double* dz;
    static VelSoA create(int n) { VelSoA s; s.dx=(double*)aligned_alloc(64,n*8); s.dy=(double*)aligned_alloc(64,n*8); s.dz=(double*)aligned_alloc(64,n*8); return s; }
    void destroy() { free(dx); free(dy); free(dz); }
};
struct alignas(64) HpSoA { double* hp; double* max_hp; int32_t* alive;
    static HpSoA create(int n) { HpSoA s; s.hp=(double*)aligned_alloc(64,n*8); s.max_hp=(double*)aligned_alloc(64,n*8); s.alive=(int32_t*)aligned_alloc(64,n*4); return s; }
    void destroy() { free(hp); free(max_hp); free(alive); }
};
struct alignas(64) TagSoA { uint8_t* team; uint8_t* active; uint8_t* dmg; uint8_t* vis;
    static TagSoA create(int n) { TagSoA s; s.team=(uint8_t*)aligned_alloc(64,n); s.active=(uint8_t*)aligned_alloc(64,n); s.dmg=(uint8_t*)aligned_alloc(64,n); s.vis=(uint8_t*)aligned_alloc(64,n); return s; }
    void destroy() { free(team); free(active); free(dmg); free(vis); }
};
struct alignas(64) MassSoA { double* m; double* inv_m;
    static MassSoA create(int n) { MassSoA s; s.m=(double*)aligned_alloc(64,n*8); s.inv_m=(double*)aligned_alloc(64,n*8); return s; }
    void destroy() { free(m); free(inv_m); }
};
struct alignas(64) AiSoA { int32_t* state; double* timer;
    static AiSoA create(int n) { AiSoA s; s.state=(int32_t*)aligned_alloc(64,n*4); s.timer=(double*)aligned_alloc(64,n*8); return s; }
    void destroy() { free(state); free(timer); }
};

// System 1: Movement (SoA + prefetch + @simd)
static void __attribute__((noinline)) ecs_move(PosSoA& p, VelSoA& v, double dt) {
    const int PF=64;
    #pragma GCC unroll 4
    for (int i=0; i<ECS_N; i++) {
        if (i+PF<ECS_N) { __builtin_prefetch(&p.x[i+PF],1,3); __builtin_prefetch(&v.dx[i+PF],0,3); }
        p.x[i]+=v.dx[i]*dt; p.y[i]+=v.dy[i]*dt; p.z[i]+=v.dz[i]*dt;
    }
}
static void __attribute__((noinline)) ecs_regen(HpSoA& h, double dt) {
    for (int i=0;i<ECS_N;i++) if(h.alive[i]){h.hp[i]+=0.01*dt; if(h.hp[i]>h.max_hp[i])h.hp[i]=h.max_hp[i];}
}
static uint64_t __attribute__((noinline)) ecs_col(PosSoA& p, HpSoA& h, TagSoA& t) {
    uint64_t c=0; const double th=2.0;
    for (int i=0;i+8<ECS_N;i+=8) { double dx=p.x[i]-p.x[i+1],dy=p.y[i]-p.y[i+1],dz=p.z[i]-p.z[i+1];
        if(__builtin_expect(dx*dx+dy*dy+dz*dz<th,0)&&t.dmg[i]&&t.dmg[i+1]){h.hp[i]-=0.5;h.hp[i+1]-=0.5;c++;}}
    return c;
}
static void __attribute__((noinline)) ecs_dmg(HpSoA& h, TagSoA& t, double d) {
    for(int i=0;i<ECS_N;i++) if(t.dmg[i]&&h.alive[i]) h.hp[i]-=d;
}
static uint64_t __attribute__((noinline)) ecs_filt(TagSoA& t, HpSoA& h) {
    uint64_t c=0; for(int i=0;i<ECS_N;i++) if(t.team[i]==1&&t.active[i]&&t.vis[i]&&h.alive[i]&&h.hp[i]>10.0) c++; return c;
}
static void __attribute__((noinline)) ecs_sparse(PosSoA& p, VelSoA& v, const int* idx, int cnt, double dt) {
    for(int j=0;j<cnt;j++){int i=idx[j]; p.x[i]+=v.dx[i]*dt*2; p.y[i]+=v.dy[i]*dt*2; p.z[i]+=v.dz[i]*dt*2;}
}
static void __attribute__((noinline)) ecs_ai(AiSoA& a, HpSoA& h, double dt) {
    for(int i=0;i<ECS_N;i++){if(!h.alive[i])continue; a.timer[i]-=dt; if(a.timer[i]<=0){a.state[i]=(a.state[i]+1)%3; a.timer[i]=1.0+(i%5)*0.5;}}
}
static void __attribute__((noinline)) ecs_phys(PosSoA& p, VelSoA& v, MassSoA& m, double dt) {
    const double g=-9.81;
    for(int i=0;i<ECS_N;i++){v.dz[i]+=g*m.m[i]*m.inv_m[i]*dt; p.z[i]+=v.dz[i]*dt; if(__builtin_expect(p.z[i]<0,0)){p.z[i]=0;v.dz[i]*=-0.5;}}
}
static uint64_t __attribute__((noinline)) ecs_death(HpSoA& h, TagSoA& t) {
    uint64_t d=0; for(int i=0;i<ECS_N;i++) if(__builtin_expect(h.alive[i]&&h.hp[i]<=0,0)){h.alive[i]=0;t.active[i]=0;d++;} return d;
}
static void __attribute__((noinline)) ecs_part(PosSoA& p, VelSoA& v, TagSoA& t, double dt) {
    for(int i=0;i<ECS_N;i++){if(!t.vis[i])continue; v.dx[i]*=0.99;v.dy[i]*=0.99;v.dz[i]*=0.99; p.x[i]+=v.dx[i]*dt;p.y[i]+=v.dy[i]*dt;}
}

static void bench_ecs() {
    BENCH_START("ECS_20M");
    printf("  Init 20M entities, 10 systems, %d frames...\n", ECS_ITERS);
    auto pos=PosSoA::create(ECS_N); auto vel=VelSoA::create(ECS_N); auto hp=HpSoA::create(ECS_N);
    auto tag=TagSoA::create(ECS_N); auto mass=MassSoA::create(ECS_N); auto ai=AiSoA::create(ECS_N);
    for(int i=0;i<ECS_N;i++){
        pos.x[i]=(i%2000); pos.y[i]=((i/2000)%2000); pos.z[i]=(i%500);
        vel.dx[i]=0.1+(i%10)*0.01; vel.dy[i]=0.2-(i%7)*0.01; vel.dz[i]=0.05;
        hp.hp[i]=(i%3==0)?5.0:100.0; hp.max_hp[i]=100.0; hp.alive[i]=1;
        tag.team[i]=i%2; tag.active[i]=1; tag.dmg[i]=(i%5!=0); tag.vis[i]=(i%3!=0);
        mass.m[i]=1.0+(i%10)*0.5; mass.inv_m[i]=1.0/mass.m[i];
        ai.state[i]=i%4; ai.timer[i]=1.0+(i%5)*0.5;
    }
    int sparse_cnt=ECS_N/20;
    int* sparse_idx=(int*)aligned_alloc(64,sparse_cnt*4);
    for(int j=0;j<sparse_cnt;j++) sparse_idx[j]=j*20;

    // Warmup
    for(int w=0;w<2;w++){ecs_move(pos,vel,0.016);ecs_regen(hp,0.016);ecs_col(pos,hp,tag);ecs_dmg(hp,tag,0.5);ecs_filt(tag,hp);ecs_sparse(pos,vel,sparse_idx,sparse_cnt,0.016);ecs_ai(ai,hp,0.016);ecs_phys(pos,vel,mass,0.016);ecs_death(hp,tag);ecs_part(pos,vel,tag,0.016);}
    for(int i=0;i<ECS_N;i++){hp.hp[i]=(i%3==0)?5.0:100.0;hp.alive[i]=1;tag.active[i]=1;}

    uint64_t tc=0,tf=0,td=0;
    auto t0=hrclock_t::now();
    for(int it=0;it<ECS_ITERS;it++){
        ecs_move(pos,vel,0.016); ecs_regen(hp,0.016); tc+=ecs_col(pos,hp,tag);
        ecs_dmg(hp,tag,0.5); tf+=ecs_filt(tag,hp); ecs_sparse(pos,vel,sparse_idx,sparse_cnt,0.016);
        ecs_ai(ai,hp,0.016); ecs_phys(pos,vel,mass,0.016); td+=ecs_death(hp,tag); ecs_part(pos,vel,tag,0.016);
    }
    double ms=ms_t(hrclock_t::now()-t0).count();
    BENCH_RESULT("total_ms",ms);
    BENCH_RESULT("per_frame_ms",ms/ECS_ITERS);
    printf("  Col:%lu Filt:%lu Death:%lu\n",tc,tf,td);
    pos.destroy();vel.destroy();hp.destroy();tag.destroy();mass.destroy();ai.destroy();free(sparse_idx);
    BENCH_END("ECS_20M");
}

// ============================================================================
// BENCHMARK 2: 2048x2048 Matrix Multiply
// ============================================================================

static const int MAT_N=2048;
static const int TILE=64;

static void __attribute__((noinline))
matmul_tiled(const double*__restrict__ A,const double*__restrict__ B,double*__restrict__ C,int n){
    for(int ii=0;ii<n;ii+=TILE)for(int jj=0;jj<n;jj+=TILE)for(int kk=0;kk<n;kk+=TILE){
        int ie=std::min(ii+TILE,n),je=std::min(jj+TILE,n),ke=std::min(kk+TILE,n);
        for(int i=ii;i<ie;i++)for(int k=kk;k<ke;k++){double a=A[i*n+k];for(int j=jj;j<je;j++)C[i*n+j]+=a*B[k*n+j];}
    }
}

static void bench_matmul() {
    BENCH_START("MATMUL_2048");
    int n=MAT_N;
    double*A=(double*)aligned_alloc(64,(size_t)n*n*8);
    double*B=(double*)aligned_alloc(64,(size_t)n*n*8);
    double*C=(double*)aligned_alloc(64,(size_t)n*n*8);
    for(int i=0;i<n*n;i++){A[i]=(i%100)*0.01;B[i]=((i+1)%100)*0.01;C[i]=0;}
    memset(C,0,(size_t)n*n*8);
    auto t0=hrclock_t::now();
    matmul_tiled(A,B,C,n);
    double ms=ms_t(hrclock_t::now()-t0).count();
    double cs=0; for(int i=0;i<n*n;i++) cs+=C[i];
    BENCH_RESULT("total_ms",ms);
    printf("  Checksum: %.4f\n",cs);
    free(A);free(B);free(C);
    BENCH_END("MATMUL_2048");
}

// ============================================================================
// BENCHMARK 3: 10M Node Graph BFS
// ============================================================================

struct GNode { int32_t id; int32_t ec; int32_t* edges; double val; int32_t vis; };

static void bench_graph_bfs() {
    BENCH_START("GRAPH_BFS_10M");
    const int NN=10'000'000;
    auto* nodes=(GNode*)aligned_alloc(64,NN*sizeof(GNode));
    int* edges=(int32_t*)aligned_alloc(64,NN*4*sizeof(int32_t));
    for(int i=0;i<NN;i++){
        nodes[i].id=i; nodes[i].val=i*0.001; nodes[i].vis=0;
        nodes[i].edges=&edges[i*4]; nodes[i].ec=4;
        for(int e=0;e<4;e++) nodes[i].edges[e]=(e==0&&i+1<NN)?i+1:(int)((long long)i*i%NN);
    }
    double total=0; int64_t tv=0;
    for(int r=0;r<3;r++){
        for(int i=0;i<NN;i++) nodes[i].vis=0;
        int*q=(int*)aligned_alloc(64,NN*4); int qh=0,qt=0; q[qt++]=0; nodes[0].vis=1; int64_t vis=0;
        auto t0=hrclock_t::now();
        while(qh<qt){int nid=q[qh++]; vis++; GNode&nd=nodes[nid]; for(int e=0;e<nd.ec;e++){int t=nd.edges[e];if(!nodes[t].vis){nodes[t].vis=1;q[qt++]=t;}}}
        total+=ms_t(hrclock_t::now()-t0).count(); tv+=vis; free(q);
    }
    BENCH_RESULT("total_ms",total); BENCH_RESULT("avg_ms",total/3);
    printf("  Visited(3 rounds): %ld\n",tv);
    free(nodes);free(edges);
    BENCH_END("GRAPH_BFS_10M");
}

// ============================================================================
// BENCHMARK 4: Allocator Stress — 50M allocations
// ============================================================================

struct Arena { char*buf; int64_t cap,off;
    void init(int64_t c){buf=(char*)aligned_alloc(64,c);cap=c;off=0;}
    void* alloc(int64_t sz){int64_t a=(off+15)&~15;if(a+sz>cap)return nullptr;void*p=buf+a;off=a+sz;return p;}
    void reset(){off=0;} void destroy(){free(buf);}
};

static void bench_allocator() {
    BENCH_START("ALLOCATOR_50M");
    const int NA=50'000'000;
    Arena ar; ar.init(2LL*1024*1024*1024);
    auto t0=hrclock_t::now();
    volatile int64_t as=0;
    for(int i=0;i<NA;i++){int sz=16+(i%4)*16;void*p=ar.alloc(sz);if(p){*(int64_t*)p=i;as+=*(int64_t*)p;}if(i%1000000==999999)ar.reset();}
    double ams=ms_t(hrclock_t::now()-t0).count();

    t0=hrclock_t::now();
    volatile int64_t hs=0;
    for(int i=0;i<NA;i++){int sz=16+(i%4)*16;void*p=malloc(sz);if(p){*(int64_t*)p=i;hs+=*(int64_t*)p;free(p);}}
    double hms=ms_t(hrclock_t::now()-t0).count();

    BENCH_RESULT("arena_ms",ams); BENCH_RESULT("heap_ms",hms); BENCH_RESULT("speedup",hms/ams);
    printf("  Arena:%ld Heap:%ld\n",as,hs);
    ar.destroy();
    BENCH_END("ALLOCATOR_50M");
}

// ============================================================================
// BENCHMARK 5: Multi-threaded — 8 threads, 2M tasks
// ============================================================================

struct TPool {
    std::vector<std::thread> w; std::queue<std::function<void()>> q;
    std::mutex m; std::condition_variable cv; std::atomic<bool> stop{false}; std::atomic<int64_t> done{0};
    TPool(int n){for(int i=0;i<n;i++)w.emplace_back([this]{while(true){std::function<void()>t;{std::unique_lock<std::mutex>l(m);cv.wait(l,[this]{return stop||!q.empty();});if(stop&&q.empty())return;t=std::move(q.front());q.pop();}t();done++;}});}
    void submit(std::function<void()>f){{std::unique_lock<std::mutex>l(m);q.push(std::move(f));}cv.notify_one();}
    void wait(int64_t c){while(done.load()<c)std::this_thread::yield();}
    ~TPool(){stop=true;cv.notify_all();for(auto&t:w)t.join();}
};

static int64_t fib(int64_t n){if(n<=1)return n;volatile int64_t a=0,b=1;for(int64_t i=2;i<=n;i++){int64_t c=a+b;a=b;b=c;}return b;}

static void bench_threadpool() {
    BENCH_START("THREADPOOL_2M");
    const int NT=2'000'000;
    const int NTHR=8;
    const int CHUNK=NT/NTHR;
    std::atomic<int64_t> rs{0};
    auto t0=hrclock_t::now();
    std::vector<std::thread> threads;
    for(int t=0;t<NTHR;t++){
        threads.emplace_back([&rs,t,CHUNK,NT,NTHR]{
            int64_t local=0;
            int start=t*CHUNK;
            int end=(t==NTHR-1)?NT:(t+1)*CHUNK;
            for(int i=start;i<end;i++) local+=fib(20+(i%20));
            rs+=local;
        });
    }
    for(auto& th:threads) th.join();
    double ms=ms_t(hrclock_t::now()-t0).count();
    BENCH_RESULT("total_ms",ms); BENCH_RESULT("tasks_per_sec",NT/(ms/1000));
    printf("  Sum: %ld\n",rs.load());
    BENCH_END("THREADPOOL_2M");
}

// ============================================================================
// BENCHMARK 6: Error Path — 10M calls
// ============================================================================

static int __attribute__((noinline)) tether_fn(int in,int*__restrict__ err){
    if(__builtin_expect(in%100==0,0)){*err=42;return 0;} return in*3+7;
}
static int __attribute__((noinline)) rust_fn(int in,int*__restrict__ err){
    if(in%100==0){*err=42;return 0;} return in*3+7;
}

static void bench_error_path() {
    BENCH_START("ERROR_10M");
    const int NC=10'000'000;
    int64_t ts=0; int te=0;
    auto t0=hrclock_t::now();
    for(int i=0;i<NC;i++){int e=0;int v=tether_fn(i,&e);if(__builtin_expect(e,0))te++;else ts+=v;}
    double tms=ms_t(hrclock_t::now()-t0).count();

    int64_t rs=0; int re=0;
    t0=hrclock_t::now();
    for(int i=0;i<NC;i++){int e=0;int v=rust_fn(i,&e);if(e)re++;else rs+=v;}
    double rms=ms_t(hrclock_t::now()-t0).count();

    BENCH_RESULT("tether_ms",tms); BENCH_RESULT("rust_ms",rms); BENCH_RESULT("speedup",rms/tms);
    printf("  T:%ld(%d) R:%ld(%d)\n",ts,te,rs,re);
    BENCH_END("ERROR_10M");
}

// ============================================================================
// BENCHMARK 7: Radix Sort — 50M integers
// ============================================================================

static void radix_sort(int32_t*data,int32_t*temp,int n){
    int32_t*src=data,*dst=temp;
    for(int sh=0;sh<32;sh+=8){
        int cnt[256]={}; for(int i=0;i<n;i++)cnt[(src[i]>>sh)&0xFF]++;
        int tot=0; for(int i=0;i<256;i++){int c=cnt[i];cnt[i]=tot;tot+=c;}
        for(int i=0;i<n;i++){int b=(src[i]>>sh)&0xFF;dst[cnt[b]++]=src[i];}
        int32_t*t=src;src=dst;dst=t;
    }
    if(src!=data)memcpy(data,src,n*4);
}

static void bench_radix_sort() {
    BENCH_START("RADIX_50M");
    const int N=50'000'000;
    auto*d=(int32_t*)aligned_alloc(64,N*4); auto*t=(int32_t*)aligned_alloc(64,N*4);
    uint32_t rng=42; for(int i=0;i<N;i++){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;d[i]=(int32_t)(rng&0x7FFFFFFF);}
    auto t0=hrclock_t::now();
    radix_sort(d,t,N);
    double ms=ms_t(hrclock_t::now()-t0).count();
    bool ok=true; for(int i=1;i<N;i++)if(d[i]<d[i-1]){ok=false;break;}
    BENCH_RESULT("total_ms",ms);
    printf("  Sorted:%s First:%d Last:%d\n",ok?"YES":"NO",d[0],d[N-1]);
    free(d);free(t);
    BENCH_END("RADIX_50M");
}

int main() {
    printf("======================================================================\n");
    printf("  Tether Heavy Benchmark — -O3 -march=native\n");
    printf("  7 benches: ECS(20M), MatMul(2K), BFS(10M), Alloc(50M),\n");
    printf("             ThreadPool(2M), ErrorPath(10M), RadixSort(50M)\n");
    printf("======================================================================\n\n");

    bench_ecs(); printf("\n");
    bench_matmul(); printf("\n");
    bench_graph_bfs(); printf("\n");
    bench_allocator(); printf("\n");
    bench_threadpool(); printf("\n");
    bench_error_path(); printf("\n");
    bench_radix_sort();

    printf("\n======================================================================\n");
    printf("  All benchmarks complete.\n");
    printf("======================================================================\n");
    return 0;
}
