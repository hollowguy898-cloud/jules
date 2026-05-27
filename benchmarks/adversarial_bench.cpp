// ============================================================================
// ADVERSARIAL BENCHMARK — "The Compiler vs Reality"
//
// This benchmark tests what actually breaks systems languages and ECS frameworks:
//   1. Cache-hostile random graph traversal (power-law, no spatial locality)
//   2. Pointer-chasing ECS (dynamic entity chains, no stable layout)
//   3. Dynamic structural mutation (spawn/despawn, add/remove components)
//   4. Branch explosion physics (clustered densities, type-dependent collision)
//   5. Atomic contention + false sharing (multi-threaded hot cells)
//   6. Mixed-mode: all of the above combined
//
// Compares: AoS vs SoA vs Hybrid (Tether-style) for each scenario
//
// Compile:
//   g++ -O3 -march=native -ffast-math -std=c++20 -pthread -o adv_bench adversarial_benchmark.cpp
//
// Memory: ~5GB peak at default scale. Adjust SCALE if needed.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <random>
#include <numeric>

using namespace std;

// ============================================================================
// CONFIGURATION — scaled for 8GB / 4-core
// ============================================================================
static constexpr int N_GRAPH      = 2'000'000;   // 2M nodes for graph (graph gen is expensive)
static constexpr int N_ECS        = 5'000'000;   // 5M entities for pointer-chasing
static constexpr int N_MUT_ECS    = 3'000'000;   // 3M for structural mutation
static constexpr int N_COLL       = 5'000'000;   // 5M collision entities
static constexpr int N_ATOMIC     = 1000;         // hot cells
static constexpr int N_MIXED      = 3'000'000;   // 3M for mixed-mode
static constexpr int N_THREADS    = 4;
static constexpr int BFS_SOURCES  = 50;           // BFS seed count

// ============================================================================
// TIMING
// ============================================================================
struct BenchResult {
    char name[64];
    double ms;
};

static vector<BenchResult> results;

static double now_ms() {
    return chrono::duration<double, milli>(
        chrono::high_resolution_clock::now().time_since_epoch()).count();
}

static void record(const char* name, double ms) {
    results.push_back({});
    auto& r = results.back();
    strncpy(r.name, name, 63); r.name[63] = 0; r.ms = ms;
}

// ============================================================================
// PRNG — xorshift64
// ============================================================================
static inline uint64_t xs64(uint64_t& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
}

// ============================================================================
// Fast power-law graph generation using Zipf sampling
// ============================================================================
// Instead of O(n*k) preferential attachment, we:
// 1. Assign degrees from Zipf distribution (O(n))
// 2. Create edges via random pairing from degree-weighted pool
// This generates a realistic power-law graph in ~seconds for 2M nodes

struct FlatGraph {
    vector<int> offsets;  // [n+1]
    vector<int> neighbors; // [total_edges]
    int n;
    long long total_edges;
};

static FlatGraph generatePowerLawGraph(int n, uint64_t seed = 12345) {
    FlatGraph fg;
    fg.n = n;
    
    // Step 1: Assign degrees using fast Zipf approximation
    // P(k) ~ C / k^gamma, gamma=2.5, max_degree=50, min_degree=1
    vector<int> degrees(n);
    mt19937_64 rng(seed);
    
    // Precompute CDF for Zipf with gamma=2.5, k=1..50
    double gamma = 2.5;
    int max_k = 50;
    double cdf[51] = {0};
    double sum = 0;
    for (int k = 1; k <= max_k; k++) {
        sum += 1.0 / pow((double)k, gamma);
        cdf[k] = sum;
    }
    for (int k = 1; k <= max_k; k++) cdf[k] /= sum;
    
    long long total_deg = 0;
    uniform_real_distribution<double> udist(0.0, 1.0);
    for (int i = 0; i < n; i++) {
        double u = udist(rng);
        int deg = 1;
        for (int k = 1; k <= max_k; k++) {
            if (u <= cdf[k]) { deg = k; break; }
        }
        degrees[i] = deg;
        total_deg += deg;
    }
    
    // Step 2: Build adjacency using fast random edge assignment
    // For each node, create `degree[i]` random edges (allowing some duplicates)
    fg.offsets.resize(n + 1, 0);
    fg.offsets[0] = 0;
    for (int i = 0; i < n; i++) fg.offsets[i+1] = fg.offsets[i] + degrees[i];
    fg.total_edges = fg.offsets[n];
    fg.neighbors.resize(fg.total_edges);
    
    // Fill edges: for each node i, assign degree[i] random neighbors
    // Using a fast random target weighted by degree (simplified BA)
    // We just pick random targets — this gives ~random graph with correct degree sequence
    for (int i = 0; i < n; i++) {
        int base = fg.offsets[i];
        for (int d = 0; d < degrees[i]; d++) {
            // Pick target: slight bias toward high-degree nodes
            // Using rejection sampling with degree weighting
            int target;
            do {
                target = (int)(rng() % n);
            } while (target == i);
            fg.neighbors[base + d] = target;
        }
    }
    
    return fg;
}

// ============================================================================
// SCENARIO 1: Cache-Hostile Random Graph Traversal
// ============================================================================

struct NodeAoS {
    int id, value, visited;
    int payload[4]; // cold data — never touched in BFS
};

struct NodeHybrid {
    int id, value, visited; // hot fields only — 12 bytes
};

static void scenario1_graph_traversal() {
    printf("\n========================================================================\n");
    printf("  SCENARIO 1: Cache-Hostile Random Graph Traversal\n");
    printf("  %d nodes, power-law (gamma=2.5), BFS from %d seeds\n", N_GRAPH, BFS_SOURCES);
    printf("========================================================================\n\n");
    
    printf("  Generating power-law graph...\n");
    double t0 = now_ms();
    auto fg = generatePowerLawGraph(N_GRAPH);
    printf("  Graph built: %.0f ms | %lld edges | avg deg %.1f\n\n",
           now_ms() - t0, fg.total_edges, (double)fg.total_edges / N_GRAPH);
    
    // ---- AoS BFS ----
    {
        int N = N_GRAPH;
        NodeAoS* nodes = (NodeAoS*)aligned_alloc(64, (size_t)N * sizeof(NodeAoS));
        for (int i = 0; i < N; i++) { nodes[i].id = i; nodes[i].value = 0; nodes[i].visited = 0; }
        
        double t1 = now_ms();
        vector<int> queue;
        queue.reserve(N);
        int visited_count = 0;
        uint64_t seed = 77;
        
        for (int s = 0; s < BFS_SOURCES; s++) {
            int src = (int)(xs64(seed) % N);
            if (nodes[src].visited) continue;
            nodes[src].visited = 1; nodes[src].value = 1; visited_count++;
            queue.push_back(src);
            
            for (size_t qi = 0; qi < queue.size(); qi++) {
                int u = queue[qi];
                for (int ei = fg.offsets[u]; ei < fg.offsets[u+1]; ei++) {
                    int v = fg.neighbors[ei];
                    if (!nodes[v].visited) {
                        nodes[v].visited = 1;
                        nodes[v].value = nodes[u].value + 1;
                        visited_count++;
                        queue.push_back(v);
                    }
                }
            }
        }
        double ms = now_ms() - t1;
        record("S1-AoS-BFS", ms);
        printf("  AoS BFS:     %7.1f ms | visited %d/%d | %zuB/node\n",
               ms, visited_count, N, sizeof(NodeAoS));
        free(nodes);
    }
    
    // ---- SoA BFS ----
    {
        int N = N_GRAPH;
        int* val  = (int*)aligned_alloc(64, (size_t)N * sizeof(int));
        int* vis  = (int*)aligned_alloc(64, (size_t)N * sizeof(int));
        int* pay  = (int*)aligned_alloc(64, (size_t)N * 4 * sizeof(int));
        memset(vis, 0, (size_t)N * sizeof(int));
        
        double t1 = now_ms();
        vector<int> queue;
        queue.reserve(N);
        int visited_count = 0;
        uint64_t seed = 77;
        
        for (int s = 0; s < BFS_SOURCES; s++) {
            int src = (int)(xs64(seed) % N);
            if (vis[src]) continue;
            vis[src] = 1; val[src] = 1; visited_count++;
            queue.push_back(src);
            
            for (size_t qi = 0; qi < queue.size(); qi++) {
                int u = queue[qi];
                for (int ei = fg.offsets[u]; ei < fg.offsets[u+1]; ei++) {
                    int v = fg.neighbors[ei];
                    if (!vis[v]) {
                        vis[v] = 1; val[v] = val[u] + 1; visited_count++;
                        queue.push_back(v);
                    }
                }
            }
        }
        double ms = now_ms() - t1;
        record("S1-SoA-BFS", ms);
        printf("  SoA BFS:     %7.1f ms | visited %d/%d | separate arrays\n",
               ms, visited_count, N);
        free(val); free(vis); free(pay);
    }
    
    // ---- Hybrid BFS (Tether-style) ----
    {
        int N = N_GRAPH;
        NodeHybrid* hot  = (NodeHybrid*)aligned_alloc(64, (size_t)N * sizeof(NodeHybrid));
        int* cold = (int*)aligned_alloc(64, (size_t)N * 4 * sizeof(int));
        for (int i = 0; i < N; i++) { hot[i].id = i; hot[i].value = 0; hot[i].visited = 0; }
        
        double t1 = now_ms();
        vector<int> queue;
        queue.reserve(N);
        int visited_count = 0;
        uint64_t seed = 77;
        
        for (int s = 0; s < BFS_SOURCES; s++) {
            int src = (int)(xs64(seed) % N);
            if (hot[src].visited) continue;
            hot[src].visited = 1; hot[src].value = 1; visited_count++;
            queue.push_back(src);
            
            for (size_t qi = 0; qi < queue.size(); qi++) {
                int u = queue[qi];
                for (int ei = fg.offsets[u]; ei < fg.offsets[u+1]; ei++) {
                    int v = fg.neighbors[ei];
                    if (!hot[v].visited) {
                        hot[v].visited = 1; hot[v].value = hot[u].value + 1; visited_count++;
                        queue.push_back(v);
                    }
                }
            }
        }
        double ms = now_ms() - t1;
        record("S1-Hybrid-BFS", ms);
        printf("  Hybrid BFS:  %7.1f ms | visited %d/%d | %zuB hot + cold sep\n",
               ms, visited_count, N, sizeof(NodeHybrid));
        free(hot); free(cold);
    }
}

// ============================================================================
// SCENARIO 2: Pointer-Chasing ECS
// ============================================================================

struct PtrEntAoS {
    float x, y, vx, vy;
    int refs[8];
    int n_refs;
    int type;
    int alive;
};

struct PtrEntHybrid {
    float x, y, vx, vy;
    int type;
    int alive;
};

static void scenario2_pointer_chasing() {
    printf("\n========================================================================\n");
    printf("  SCENARIO 2: Pointer-Chasing ECS\n");
    printf("  %d entities, 3-8 refs each, chain depth 4, 3 frames\n", N_ECS);
    printf("========================================================================\n\n");
    
    // ---- AoS ----
    {
        int N = N_ECS;
        PtrEntAoS* e = (PtrEntAoS*)aligned_alloc(64, (size_t)N * sizeof(PtrEntAoS));
        uint64_t seed = 42;
        for (int i = 0; i < N; i++) {
            e[i].x = (float)(xs64(seed) % 100000);
            e[i].y = (float)(xs64(seed) % 100000);
            e[i].vx = ((float)(xs64(seed)%200)-100)*0.1f;
            e[i].vy = ((float)(xs64(seed)%200)-100)*0.1f;
            e[i].type = (int)(xs64(seed) % 8);
            e[i].alive = 1;
            e[i].n_refs = 3 + (int)(xs64(seed) % 6);
            for (int r = 0; r < 8; r++) e[i].refs[r] = (r < e[i].n_refs) ? (int)(xs64(seed) % N) : -1;
        }
        
        double resolve_ms = 0, mutate_ms = 0;
        float total_influence = 0;
        
        for (int frame = 0; frame < 3; frame++) {
            double t1 = now_ms();
            float influence = 0;
            for (int i = 0; i < N; i++) {
                if (!e[i].alive) continue;
                float val = e[i].x + e[i].y;
                int cur = i;
                for (int depth = 0; depth < 4; depth++) {
                    if (e[cur].n_refs == 0) break;
                    int next = e[cur].refs[depth % e[cur].n_refs];
                    if (next < 0 || next >= N || !e[next].alive) break;
                    switch (e[cur].type) {
                        case 0: val += e[next].vx * 0.1f; break;
                        case 1: val += e[next].vy * 0.2f; break;
                        case 2: val -= e[next].x * 0.01f; break;
                        case 3: val *= (1.0f + e[next].y * 0.001f); break;
                        case 4: val += sqrtf(e[next].vx*e[next].vx + e[next].vy*e[next].vy); break;
                        case 5: val -= e[next].type * 0.5f; break;
                        case 6: val += (e[next].x - e[cur].x) * 0.01f; break;
                        case 7: val *= 0.99f; break;
                    }
                    cur = next;
                }
                influence += val;
            }
            resolve_ms += now_ms() - t1;
            total_influence += influence;
            
            double t2 = now_ms();
            for (int i = 0; i < N; i++) {
                if (xs64(seed) % 10 == 0 && e[i].alive && e[i].n_refs > 0)
                    e[i].refs[xs64(seed) % e[i].n_refs] = (int)(xs64(seed) % N);
                e[i].x += e[i].vx; e[i].y += e[i].vy;
            }
            mutate_ms += now_ms() - t2;
        }
        
        record("S2-AoS-Resolve", resolve_ms);
        record("S2-AoS-Mutate", mutate_ms);
        printf("  AoS Chase:   resolve %6.1f ms | mutate %6.1f ms | inf=%.0f | %zuB\n",
               resolve_ms, mutate_ms, total_influence, sizeof(PtrEntAoS));
        free(e);
    }
    
    // ---- SoA ----
    {
        int N = N_ECS;
        float* ex  = (float*)aligned_alloc(64, (size_t)N * sizeof(float));
        float* ey  = (float*)aligned_alloc(64, (size_t)N * sizeof(float));
        float* evx = (float*)aligned_alloc(64, (size_t)N * sizeof(float));
        float* evy = (float*)aligned_alloc(64, (size_t)N * sizeof(float));
        int* etype  = (int*)aligned_alloc(64, (size_t)N * sizeof(int));
        int* ealive = (int*)aligned_alloc(64, (size_t)N * sizeof(int));
        int* enrefs = (int*)aligned_alloc(64, (size_t)N * sizeof(int));
        int* erefs  = (int*)aligned_alloc(64, (size_t)N * 8 * sizeof(int));
        
        uint64_t seed = 42;
        for (int i = 0; i < N; i++) {
            ex[i]=(float)(xs64(seed)%100000); ey[i]=(float)(xs64(seed)%100000);
            evx[i]=((float)(xs64(seed)%200)-100)*0.1f; evy[i]=((float)(xs64(seed)%200)-100)*0.1f;
            etype[i]=(int)(xs64(seed)%8); ealive[i]=1;
            enrefs[i]=3+(int)(xs64(seed)%6);
            for (int r=0;r<8;r++) erefs[i*8+r]=(r<enrefs[i])?(int)(xs64(seed)%N):-1;
        }
        
        double resolve_ms=0, mutate_ms=0; float total_influence=0;
        for (int frame=0;frame<3;frame++) {
            double t1=now_ms(); float influence=0;
            for (int i=0;i<N;i++) {
                if(!ealive[i])continue;
                float val=ex[i]+ey[i]; int cur=i;
                for (int depth=0;depth<4;depth++) {
                    if(enrefs[cur]==0)break;
                    int next=erefs[cur*8+depth%enrefs[cur]];
                    if(next<0||next>=N||!ealive[next])break;
                    switch(etype[cur]) {
                        case 0:val+=evx[next]*0.1f;break; case 1:val+=evy[next]*0.2f;break;
                        case 2:val-=ex[next]*0.01f;break; case 3:val*=(1.0f+ey[next]*0.001f);break;
                        case 4:val+=sqrtf(evx[next]*evx[next]+evy[next]*evy[next]);break;
                        case 5:val-=etype[next]*0.5f;break;
                        case 6:val+=(ex[next]-ex[cur])*0.01f;break; case 7:val*=0.99f;break;
                    }
                    cur=next;
                }
                influence+=val;
            }
            resolve_ms+=now_ms()-t1; total_influence+=influence;
            
            double t2=now_ms();
            for (int i=0;i<N;i++) {
                if(xs64(seed)%10==0&&ealive[i]&&enrefs[i]>0) erefs[i*8+xs64(seed)%enrefs[i]]=(int)(xs64(seed)%N);
                ex[i]+=evx[i];ey[i]+=evy[i];
            }
            mutate_ms+=now_ms()-t2;
        }
        record("S2-SoA-Resolve", resolve_ms);
        record("S2-SoA-Mutate", mutate_ms);
        printf("  SoA Chase:   resolve %6.1f ms | mutate %6.1f ms | inf=%.0f | 8 arrays\n",
               resolve_ms, mutate_ms, total_influence);
        free(ex);free(ey);free(evx);free(evy);free(etype);free(ealive);free(enrefs);free(erefs);
    }
    
    // ---- Hybrid ----
    {
        int N = N_ECS;
        PtrEntHybrid* hot = (PtrEntHybrid*)aligned_alloc(64, (size_t)N * sizeof(PtrEntHybrid));
        int* enrefs = (int*)aligned_alloc(64, (size_t)N * sizeof(int));
        int* erefs  = (int*)aligned_alloc(64, (size_t)N * 8 * sizeof(int));
        
        uint64_t seed = 42;
        for (int i = 0; i < N; i++) {
            hot[i].x=(float)(xs64(seed)%100000); hot[i].y=(float)(xs64(seed)%100000);
            hot[i].vx=((float)(xs64(seed)%200)-100)*0.1f; hot[i].vy=((float)(xs64(seed)%200)-100)*0.1f;
            hot[i].type=(int)(xs64(seed)%8); hot[i].alive=1;
            enrefs[i]=3+(int)(xs64(seed)%6);
            for (int r=0;r<8;r++) erefs[i*8+r]=(r<enrefs[i])?(int)(xs64(seed)%N):-1;
        }
        
        double resolve_ms=0, mutate_ms=0; float total_influence=0;
        for (int frame=0;frame<3;frame++) {
            double t1=now_ms(); float influence=0;
            for (int i=0;i<N;i++) {
                if(!hot[i].alive)continue;
                float val=hot[i].x+hot[i].y; int cur=i;
                for (int depth=0;depth<4;depth++) {
                    if(enrefs[cur]==0)break;
                    int next=erefs[cur*8+depth%enrefs[cur]];
                    if(next<0||next>=N||!hot[next].alive)break;
                    switch(hot[cur].type) {
                        case 0:val+=hot[next].vx*0.1f;break; case 1:val+=hot[next].vy*0.2f;break;
                        case 2:val-=hot[next].x*0.01f;break; case 3:val*=(1.0f+hot[next].y*0.001f);break;
                        case 4:val+=sqrtf(hot[next].vx*hot[next].vx+hot[next].vy*hot[next].vy);break;
                        case 5:val-=hot[next].type*0.5f;break;
                        case 6:val+=(hot[next].x-hot[cur].x)*0.01f;break; case 7:val*=0.99f;break;
                    }
                    cur=next;
                }
                influence+=val;
            }
            resolve_ms+=now_ms()-t1; total_influence+=influence;
            
            double t2=now_ms();
            for (int i=0;i<N;i++) {
                if(xs64(seed)%10==0&&hot[i].alive&&enrefs[i]>0) erefs[i*8+xs64(seed)%enrefs[i]]=(int)(xs64(seed)%N);
                hot[i].x+=hot[i].vx;hot[i].y+=hot[i].vy;
            }
            mutate_ms+=now_ms()-t2;
        }
        record("S2-Hybrid-Resolve", resolve_ms);
        record("S2-Hybrid-Mutate", mutate_ms);
        printf("  Hybrid Chase:resolve %6.1f ms | mutate %6.1f ms | inf=%.0f | %zuB hot+refs\n",
               resolve_ms, mutate_ms, total_influence, sizeof(PtrEntHybrid));
        free(hot);free(enrefs);free(erefs);
    }
}

// ============================================================================
// SCENARIO 3: Dynamic Structural Mutation
// ============================================================================

struct MutEntAoS { float x, y; int components, alive; };
struct MutEntHybrid { float x, y; int alive; };

static void scenario3_structural_mutation() {
    printf("\n========================================================================\n");
    printf("  SCENARIO 3: Dynamic Structural Mutation\n");
    printf("  %d entities, spawn/despawn + component toggle, %d frames\n", N_MUT_ECS, 5);
    printf("========================================================================\n\n");
    
    int N = N_MUT_ECS;
    int CAP = N + N/4;
    
    // ---- AoS ----
    {
        MutEntAoS* e = (MutEntAoS*)aligned_alloc(64, (size_t)CAP * sizeof(MutEntAoS));
        uint64_t seed = 99; int alive_count = N;
        for (int i=0;i<CAP;i++) { e[i].x=(i<N)?(float)(xs64(seed)%10000):0; e[i].y=(i<N)?(float)(xs64(seed)%10000):0; e[i].components=(i<N)?0xFF:0; e[i].alive=(i<N)?1:0; }
        
        double t1=now_ms(); float total_movement=0;
        for (int frame=0;frame<5;frame++) {
            int to_kill=alive_count/10;
            for (int k=0;k<to_kill;k++){int idx=(int)(xs64(seed)%CAP);if(e[idx].alive){e[idx].alive=0;alive_count--;}}
            for (int s=0;s<to_kill;s++){int idx=(int)(xs64(seed)%CAP);if(!e[idx].alive){e[idx].alive=1;e[idx].x=(float)(xs64(seed)%10000);e[idx].y=(float)(xs64(seed)%10000);e[idx].components=0x3F;alive_count++;}}
            for (int i=0;i<CAP;i++){if(!e[i].alive)continue;if(xs64(seed)%10<3)e[i].components^=(1<<(xs64(seed)%8));}
            float movement=0;
            for (int i=0;i<CAP;i++){
                if(!e[i].alive||(e[i].components&0x0F)!=0x0F)continue;
                float dx=0,dy=0;
                if(e[i].components&0x01)dx+=0.1f; if(e[i].components&0x02)dy+=0.1f;
                if(e[i].components&0x04)dx*=1.5f; if(e[i].components&0x08)dy*=1.5f;
                if(e[i].components&0x10){dx*=0.9f;dy*=0.9f;} if(e[i].components&0x20){dx+=0.5f;dy-=0.3f;}
                if(e[i].components&0x40){dx=-dx;dy=-dy;} if(e[i].components&0x80){float t=dx;dx=dy;dy=t;}
                e[i].x+=dx;e[i].y+=dy; movement+=dx+dy;
            }
            total_movement+=movement;
        }
        double ms=now_ms()-t1;
        record("S3-AoS-Mutation", ms);
        printf("  AoS Mutation:  %7.1f ms | alive=%d | mov=%.0f\n", ms, alive_count, total_movement);
        free(e);
    }
    
    // ---- SoA ----
    {
        float* ex=(float*)aligned_alloc(64,(size_t)CAP*sizeof(float));
        float* ey=(float*)aligned_alloc(64,(size_t)CAP*sizeof(float));
        int* ecomp=(int*)aligned_alloc(64,(size_t)CAP*sizeof(int));
        int* ealive=(int*)aligned_alloc(64,(size_t)CAP*sizeof(int));
        uint64_t seed=99; int alive_count=N;
        for (int i=0;i<CAP;i++){ex[i]=(i<N)?(float)(xs64(seed)%10000):0;ey[i]=(i<N)?(float)(xs64(seed)%10000):0;ecomp[i]=(i<N)?0xFF:0;ealive[i]=(i<N)?1:0;}
        
        double t1=now_ms(); float total_movement=0;
        for (int frame=0;frame<5;frame++){
            int to_kill=alive_count/10;
            for (int k=0;k<to_kill;k++){int idx=(int)(xs64(seed)%CAP);if(ealive[idx]){ealive[idx]=0;alive_count--;}}
            for (int s=0;s<to_kill;s++){int idx=(int)(xs64(seed)%CAP);if(!ealive[idx]){ealive[idx]=1;ex[idx]=(float)(xs64(seed)%10000);ey[idx]=(float)(xs64(seed)%10000);ecomp[idx]=0x3F;alive_count++;}}
            for (int i=0;i<CAP;i++){if(!ealive[i])continue;if(xs64(seed)%10<3)ecomp[i]^=(1<<(xs64(seed)%8));}
            float movement=0;
            for (int i=0;i<CAP;i++){
                if(!ealive[i]||(ecomp[i]&0x0F)!=0x0F)continue;
                float dx=0,dy=0;
                if(ecomp[i]&0x01)dx+=0.1f; if(ecomp[i]&0x02)dy+=0.1f;
                if(ecomp[i]&0x04)dx*=1.5f; if(ecomp[i]&0x08)dy*=1.5f;
                if(ecomp[i]&0x10){dx*=0.9f;dy*=0.9f;} if(ecomp[i]&0x20){dx+=0.5f;dy-=0.3f;}
                if(ecomp[i]&0x40){dx=-dx;dy=-dy;} if(ecomp[i]&0x80){float t=dx;dx=dy;dy=t;}
                ex[i]+=dx;ey[i]+=dy; movement+=dx+dy;
            }
            total_movement+=movement;
        }
        double ms=now_ms()-t1;
        record("S3-SoA-Mutation", ms);
        printf("  SoA Mutation:  %7.1f ms | alive=%d | mov=%.0f\n", ms, alive_count, total_movement);
        free(ex);free(ey);free(ecomp);free(ealive);
    }
    
    // ---- Hybrid ----
    {
        MutEntHybrid* hot=(MutEntHybrid*)aligned_alloc(64,(size_t)CAP*sizeof(MutEntHybrid));
        int* ecomp=(int*)aligned_alloc(64,(size_t)CAP*sizeof(int));
        uint64_t seed=99; int alive_count=N;
        for (int i=0;i<CAP;i++){hot[i].x=(i<N)?(float)(xs64(seed)%10000):0;hot[i].y=(i<N)?(float)(xs64(seed)%10000):0;hot[i].alive=(i<N)?1:0;ecomp[i]=(i<N)?0xFF:0;}
        
        double t1=now_ms(); float total_movement=0;
        for (int frame=0;frame<5;frame++){
            int to_kill=alive_count/10;
            for (int k=0;k<to_kill;k++){int idx=(int)(xs64(seed)%CAP);if(hot[idx].alive){hot[idx].alive=0;alive_count--;}}
            for (int s=0;s<to_kill;s++){int idx=(int)(xs64(seed)%CAP);if(!hot[idx].alive){hot[idx].alive=1;hot[idx].x=(float)(xs64(seed)%10000);hot[idx].y=(float)(xs64(seed)%10000);ecomp[idx]=0x3F;alive_count++;}}
            for (int i=0;i<CAP;i++){if(!hot[i].alive)continue;if(xs64(seed)%10<3)ecomp[i]^=(1<<(xs64(seed)%8));}
            float movement=0;
            for (int i=0;i<CAP;i++){
                if(!hot[i].alive||(ecomp[i]&0x0F)!=0x0F)continue;
                float dx=0,dy=0;
                if(ecomp[i]&0x01)dx+=0.1f; if(ecomp[i]&0x02)dy+=0.1f;
                if(ecomp[i]&0x04)dx*=1.5f; if(ecomp[i]&0x08)dy*=1.5f;
                if(ecomp[i]&0x10){dx*=0.9f;dy*=0.9f;} if(ecomp[i]&0x20){dx+=0.5f;dy-=0.3f;}
                if(ecomp[i]&0x40){dx=-dx;dy=-dy;} if(ecomp[i]&0x80){float t=dx;dx=dy;dy=t;}
                hot[i].x+=dx;hot[i].y+=dy; movement+=dx+dy;
            }
            total_movement+=movement;
        }
        double ms=now_ms()-t1;
        record("S3-Hybrid-Mutation", ms);
        printf("  Hybrid Mutation:%6.1f ms | alive=%d | mov=%.0f\n", ms, alive_count, total_movement);
        free(hot);free(ecomp);
    }
}

// ============================================================================
// SCENARIO 4: Branch Explosion Physics
// ============================================================================

struct CollEntAoS { float x,y,vx,vy,radius; int type,cluster_id,alive; };
struct CollEntHybrid { float x,y,vx,vy; int type,cluster_id; };

static void scenario4_branch_explosion() {
    printf("\n========================================================================\n");
    printf("  SCENARIO 4: Branch Explosion Physics\n");
    printf("  %d entities, 50 hotspot clusters, type-dependent collision\n", N_COLL);
    printf("========================================================================\n\n");
    
    int N = N_COLL;
    float WORLD = 50000.0f;
    float CELL = 40.0f;
    int GDIM = (int)(WORLD/CELL);
    int NCELLS = GDIM*GDIM;
    
    int* flat=(int*)aligned_alloc(64,(size_t)N*sizeof(int));
    int* sorted_buf=(int*)aligned_alloc(64,(size_t)N*sizeof(int));
    int* cstart=(int*)aligned_alloc(64,(size_t)NCELLS*sizeof(int));
    int* ccount=(int*)aligned_alloc(64,(size_t)NCELLS*sizeof(int));
    int* woff=(int*)aligned_alloc(64,(size_t)NCELLS*sizeof(int));
    
    // ---- AoS ----
    {
        CollEntAoS* e=(CollEntAoS*)aligned_alloc(64,(size_t)N*sizeof(CollEntAoS));
        uint64_t seed=555;
        float cx[50],cy[50]; for (int c=0;c<50;c++){cx[c]=(float)(xs64(seed)%(int)WORLD);cy[c]=(float)(xs64(seed)%(int)WORLD);}
        for (int i=0;i<N;i++){
            if(xs64(seed)%5!=0){int c=(int)(xs64(seed)%50);e[i].x=cx[c]+(float)((int)(xs64(seed)%400)-200);e[i].y=cy[c]+(float)((int)(xs64(seed)%400)-200);e[i].cluster_id=c;}
            else{e[i].x=(float)(xs64(seed)%(int)WORLD);e[i].y=(float)(xs64(seed)%(int)WORLD);e[i].cluster_id=-1;}
            e[i].vx=((float)(xs64(seed)%200)-100)*0.05f;e[i].vy=((float)(xs64(seed)%200)-100)*0.05f;
            e[i].radius=(e[i].cluster_id>=0)?2.0f+(float)(xs64(seed)%20)*0.1f:5.0f+(float)(xs64(seed)%40)*0.1f;
            e[i].type=(int)(xs64(seed)%8);e[i].alive=1;
        }
        
        double coll_ms=0, grid_ms=0, move_ms=0; long long total_coll=0;
        for (int frame=0;frame<3;frame++){
            double tg=now_ms();
            memset(ccount,0,NCELLS*sizeof(int));
            for (int i=0;i<N;i++){if(!e[i].alive){flat[i]=-1;continue;}int gx=max(0,min((int)(e[i].x/CELL),GDIM-1));int gy=max(0,min((int)(e[i].y/CELL),GDIM-1));flat[i]=gy*GDIM+gx;ccount[gy*GDIM+gx]++;}
            cstart[0]=0;for(int i=1;i<NCELLS;i++)cstart[i]=cstart[i-1]+ccount[i-1];memcpy(woff,cstart,NCELLS*sizeof(int));
            for(int i=0;i<N;i++){if(flat[i]>=0)sorted_buf[woff[flat[i]]++]=i;}
            grid_ms+=now_ms()-tg;
            
            double tc=now_ms(); int coll=0;
            static const int d5x[]={0,1,0,1,-1},d5y[]={0,0,1,1,1};
            for(int cy=0;cy<GDIM;cy++)for(int cx=0;cx<GDIM;cx++){
                int ci=cy*GDIM+cx,st=cstart[ci],cnt=ccount[ci];if(!cnt||cnt>100)continue;
                for(int ni=0;ni<5;ni++){int nx=cx+d5x[ni],ny=cy+d5y[ni];if(nx<0||nx>=GDIM||ny<0||ny>=GDIM)continue;
                    int ni2=ny*GDIM+nx,nst=cstart[ni2],nc=ccount[ni2];if(nc>100)continue;
                    for(int ei=st;ei<st+cnt;ei++){int a=sorted_buf[ei];int j0=(ni==0)?ei+1:nst;
                        for(int ej=j0;ej<nst+nc;ej++){int b=sorted_buf[ej];
                            float dx=e[b].x-e[a].x,dy=e[b].y-e[a].y,rs=e[a].radius+e[b].radius,dsq=dx*dx+dy*dy;
                            bool sc=false;float rf=1.0f;
                            switch(e[a].type){
                                case 0:sc=(e[b].type!=0);rf=1.0f;break; case 1:sc=true;rf=0.5f;break;
                                case 2:sc=(e[b].type%2==0);rf=2.0f;break; case 3:sc=(e[b].cluster_id==e[a].cluster_id);rf=0.8f;break;
                                case 4:sc=(e[b].type!=4&&e[b].type!=5);rf=1.5f;break; case 5:sc=(e[b].radius<e[a].radius);rf=1.2f;break;
                                case 6:sc=(e[b].type>=3);rf=0.3f;break; case 7:sc=(dsq<rs*rs*0.25f);rf=3.0f;break;
                            }
                            if(sc&&dsq<rs*rs&&dsq>1e-4f){coll++;
                                float d=sqrtf(dsq),nx_c=dx/d,ny_c=dy/d;
                                float dvn=(e[a].vx-e[b].vx)*nx_c+(e[a].vy-e[b].vy)*ny_c;
                                if(dvn>0){float imp=dvn*rf;e[a].vx-=imp*nx_c;e[a].vy-=imp*ny_c;e[b].vx+=imp*nx_c;e[b].vy+=imp*ny_c;}
                                float ov=rs-d;if(ov>0){float s=ov*0.5f;e[a].x-=s*nx_c;e[a].y-=s*ny_c;e[b].x+=s*nx_c;e[b].y+=s*ny_c;}
                            }
                        }
                    }
                }
            }
            coll_ms+=now_ms()-tc; total_coll+=coll;
            
            double tm=now_ms();
            for(int i=0;i<N;i++){if(!e[i].alive)continue;e[i].x+=e[i].vx;e[i].y+=e[i].vy;
                if(e[i].x<0){e[i].x=-e[i].x;e[i].vx=-e[i].vx;}if(e[i].y<0){e[i].y=-e[i].y;e[i].vy=-e[i].vy;}
                if(e[i].x>WORLD){e[i].x=2*WORLD-e[i].x;e[i].vx=-e[i].vx;}if(e[i].y>WORLD){e[i].y=2*WORLD-e[i].y;e[i].vy=-e[i].vy;}}
            move_ms+=now_ms()-tm;
        }
        record("S4-AoS-Collision", coll_ms);
        record("S4-AoS-Grid", grid_ms);
        printf("  AoS Branch:  grid %5.0f | coll %6.0f ms (%lld) | move %5.0f ms | %zuB\n",
               grid_ms/3, coll_ms/3, total_coll/3, move_ms/3, sizeof(CollEntAoS));
        free(e);
    }
    
    // ---- Hybrid ----
    {
        CollEntHybrid* hot=(CollEntHybrid*)aligned_alloc(64,(size_t)N*sizeof(CollEntHybrid));
        float* erad=(float*)aligned_alloc(64,(size_t)N*sizeof(float));
        int* ealive=(int*)aligned_alloc(64,(size_t)N*sizeof(int));
        uint64_t seed=555;
        float cx[50],cy[50]; for(int c=0;c<50;c++){cx[c]=(float)(xs64(seed)%(int)WORLD);cy[c]=(float)(xs64(seed)%(int)WORLD);}
        for(int i=0;i<N;i++){
            if(xs64(seed)%5!=0){int c=(int)(xs64(seed)%50);hot[i].x=cx[c]+(float)((int)(xs64(seed)%400)-200);hot[i].y=cy[c]+(float)((int)(xs64(seed)%400)-200);hot[i].cluster_id=c;}
            else{hot[i].x=(float)(xs64(seed)%(int)WORLD);hot[i].y=(float)(xs64(seed)%(int)WORLD);hot[i].cluster_id=-1;}
            hot[i].vx=((float)(xs64(seed)%200)-100)*0.05f;hot[i].vy=((float)(xs64(seed)%200)-100)*0.05f;
            hot[i].type=(int)(xs64(seed)%8);
            erad[i]=(hot[i].cluster_id>=0)?2.0f+(float)(xs64(seed)%20)*0.1f:5.0f+(float)(xs64(seed)%40)*0.1f;
            ealive[i]=1;
        }
        
        double coll_ms=0,grid_ms=0,move_ms=0; long long total_coll=0;
        for(int frame=0;frame<3;frame++){
            double tg=now_ms();
            memset(ccount,0,NCELLS*sizeof(int));
            for(int i=0;i<N;i++){if(!ealive[i]){flat[i]=-1;continue;}int gx=max(0,min((int)(hot[i].x/CELL),GDIM-1));int gy=max(0,min((int)(hot[i].y/CELL),GDIM-1));flat[i]=gy*GDIM+gx;ccount[gy*GDIM+gx]++;}
            cstart[0]=0;for(int i=1;i<NCELLS;i++)cstart[i]=cstart[i-1]+ccount[i-1];memcpy(woff,cstart,NCELLS*sizeof(int));
            for(int i=0;i<N;i++){if(flat[i]>=0)sorted_buf[woff[flat[i]]++]=i;}
            grid_ms+=now_ms()-tg;
            
            double tc=now_ms(); int coll=0;
            static const int d5x[]={0,1,0,1,-1},d5y[]={0,0,1,1,1};
            for(int cy=0;cy<GDIM;cy++)for(int cx=0;cx<GDIM;cx++){
                int ci=cy*GDIM+cx,st=cstart[ci],cnt=ccount[ci];if(!cnt||cnt>100)continue;
                for(int ni=0;ni<5;ni++){int nx=cx+d5x[ni],ny=cy+d5y[ni];if(nx<0||nx>=GDIM||ny<0||ny>=GDIM)continue;
                    int ni2=ny*GDIM+nx,nst=cstart[ni2],nc=ccount[ni2];if(nc>100)continue;
                    for(int ei=st;ei<st+cnt;ei++){int a=sorted_buf[ei];int j0=(ni==0)?ei+1:nst;
                        for(int ej=j0;ej<nst+nc;ej++){int b=sorted_buf[ej];
                            float dx=hot[b].x-hot[a].x,dy=hot[b].y-hot[a].y,rs=erad[a]+erad[b],dsq=dx*dx+dy*dy;
                            bool sc=false;float rf=1.0f;
                            switch(hot[a].type){
                                case 0:sc=(hot[b].type!=0);rf=1.0f;break; case 1:sc=true;rf=0.5f;break;
                                case 2:sc=(hot[b].type%2==0);rf=2.0f;break; case 3:sc=(hot[b].cluster_id==hot[a].cluster_id);rf=0.8f;break;
                                case 4:sc=(hot[b].type!=4&&hot[b].type!=5);rf=1.5f;break; case 5:sc=(erad[b]<erad[a]);rf=1.2f;break;
                                case 6:sc=(hot[b].type>=3);rf=0.3f;break; case 7:sc=(dsq<rs*rs*0.25f);rf=3.0f;break;
                            }
                            if(sc&&dsq<rs*rs&&dsq>1e-4f){coll++;
                                float d=sqrtf(dsq),nx_c=dx/d,ny_c=dy/d;
                                float dvn=(hot[a].vx-hot[b].vx)*nx_c+(hot[a].vy-hot[b].vy)*ny_c;
                                if(dvn>0){float imp=dvn*rf;hot[a].vx-=imp*nx_c;hot[a].vy-=imp*ny_c;hot[b].vx+=imp*nx_c;hot[b].vy+=imp*ny_c;}
                                float ov=rs-d;if(ov>0){float s=ov*0.5f;hot[a].x-=s*nx_c;hot[a].y-=s*ny_c;hot[b].x+=s*nx_c;hot[b].y+=s*ny_c;}
                            }
                        }
                    }
                }
            }
            coll_ms+=now_ms()-tc; total_coll+=coll;
            
            double tm=now_ms();
            for(int i=0;i<N;i++){if(!ealive[i])continue;hot[i].x+=hot[i].vx;hot[i].y+=hot[i].vy;
                if(hot[i].x<0){hot[i].x=-hot[i].x;hot[i].vx=-hot[i].vx;}if(hot[i].y<0){hot[i].y=-hot[i].y;hot[i].vy=-hot[i].vy;}
                if(hot[i].x>WORLD){hot[i].x=2*WORLD-hot[i].x;hot[i].vx=-hot[i].vx;}if(hot[i].y>WORLD){hot[i].y=2*WORLD-hot[i].y;hot[i].vy=-hot[i].vy;}}
            move_ms+=now_ms()-tm;
        }
        record("S4-Hybrid-Collision", coll_ms);
        record("S4-Hybrid-Grid", grid_ms);
        printf("  Hybrid Branch:grid %5.0f | coll %6.0f ms (%lld) | move %5.0f ms | %zuB hot\n",
               grid_ms/3, coll_ms/3, total_coll/3, move_ms/3, sizeof(CollEntHybrid));
        free(hot);free(erad);free(ealive);
    }
    
    free(flat);free(sorted_buf);free(cstart);free(ccount);free(woff);
}

// ============================================================================
// SCENARIO 5: Atomic Contention + False Sharing
// ============================================================================

struct alignas(64) PaddedCell { atomic<int> count; atomic<float> energy; };
struct PackedCell { atomic<int> count; atomic<float> energy; };

static void scenario5_atomic_contention() {
    printf("\n========================================================================\n");
    printf("  SCENARIO 5: Atomic Contention + False Sharing\n");
    printf("  %d hot cells, %d threads, %dM ops/thread\n", N_ATOMIC, N_THREADS, 10);
    printf("========================================================================\n\n");
    
    int N = N_ATOMIC;
    int OPS = 10'000'000;
    
    // --- Padded (cache-line aligned, no false sharing) ---
    {
        PaddedCell* cells = new PaddedCell[N];
        for(int i=0;i<N;i++){cells[i].count.store(0);cells[i].energy.store(0.0f);}
        
        double t1=now_ms();
        vector<thread> threads;
        for(int t=0;t<N_THREADS;t++){
            threads.emplace_back([&,t](){
                uint64_t seed=12345+t*777;
                for(int op=0;op<OPS;op++){
                    int cell=(int)(xs64(seed)%N);
                    switch(xs64(seed)%4){
                        case 0:cells[cell].count.fetch_add(1,memory_order_relaxed);break;
                        case 1:{float e=cells[cell].energy.load(memory_order_relaxed);cells[cell].energy.store(e+0.01f,memory_order_relaxed);}break;
                        case 2:if(cells[cell].count.load(memory_order_relaxed)<1000)cells[cell].count.fetch_add(1,memory_order_relaxed);break;
                        case 3:{int old=cells[cell].count.load(memory_order_relaxed);while(!cells[cell].count.compare_exchange_weak(old,old+1,memory_order_relaxed,memory_order_relaxed));}break;
                    }
                }
            });
        }
        for(auto&th:threads)th.join();
        double ms=now_ms()-t1;
        record("S5-Padded-Atomic", ms);
        long long total=0;for(int i=0;i<N;i++)total+=cells[i].count.load();
        printf("  Padded (64B):  %7.1f ms | %lld total | %.0f Mops/s\n",
               ms, total, (double)N_THREADS*OPS/ms/1000);
        delete[] cells;
    }
    
    // --- Packed (8B — false sharing!) ---
    {
        // Allocate enough packed cells so they overlap cache lines
        PackedCell* cells = new PackedCell[N * 8];
        for(int i=0;i<N*8;i++){cells[i].count.store(0);cells[i].energy.store(0.0f);}
        
        double t1=now_ms();
        vector<thread> threads;
        for(int t=0;t<N_THREADS;t++){
            threads.emplace_back([&,t](){
                uint64_t seed=12345+t*777;
                for(int op=0;op<OPS;op++){
                    int cell=(int)(xs64(seed)%N);
                    switch(xs64(seed)%4){
                        case 0:cells[cell].count.fetch_add(1,memory_order_relaxed);break;
                        case 1:{float e=cells[cell].energy.load(memory_order_relaxed);cells[cell].energy.store(e+0.01f,memory_order_relaxed);}break;
                        case 2:if(cells[cell].count.load(memory_order_relaxed)<1000)cells[cell].count.fetch_add(1,memory_order_relaxed);break;
                        case 3:{int old=cells[cell].count.load(memory_order_relaxed);while(!cells[cell].count.compare_exchange_weak(old,old+1,memory_order_relaxed,memory_order_relaxed));}break;
                    }
                }
            });
        }
        for(auto&th:threads)th.join();
        double ms=now_ms()-t1;
        record("S5-Packed-FalseShare", ms);
        long long total=0;for(int i=0;i<N;i++)total+=cells[i].count.load();
        printf("  Packed (8B):   %7.1f ms | %lld total | %.0f Mops/s | FALSE SHARING\n",
               ms, total, (double)N_THREADS*OPS/ms/1000);
        delete[] cells;
    }
}

// ============================================================================
// SCENARIO 6: Mixed Mode — The True Final Boss
// ============================================================================

struct MixedAoS {
    float x,y,vx,vy,radius;
    int refs[4],n_refs,type,components,alive;
};

struct MixedHybrid {
    float x,y,vx,vy; int type,alive;
};

static void scenario6_mixed_mode() {
    printf("\n========================================================================\n");
    printf("  SCENARIO 6: Mixed Mode — The True Final Boss\n");
    printf("  %d entities: graph+ECS+mutation+collision+streaming+topology\n", N_MIXED);
    printf("========================================================================\n\n");
    
    int N = N_MIXED;
    int CAP = N + N/4;
    float WORLD = 30000.0f;
    float CELL = 50.0f;
    int GDIM = (int)(WORLD/CELL);
    int NCELLS = GDIM*GDIM;
    
    int* flat=(int*)aligned_alloc(64,(size_t)CAP*sizeof(int));
    int* sorted_buf=(int*)aligned_alloc(64,(size_t)CAP*sizeof(int));
    int* cstart=(int*)aligned_alloc(64,(size_t)NCELLS*sizeof(int));
    int* ccount=(int*)aligned_alloc(64,(size_t)NCELLS*sizeof(int));
    int* woff=(int*)aligned_alloc(64,(size_t)NCELLS*sizeof(int));
    
    // ---- AoS Mixed ----
    {
        MixedAoS* e=(MixedAoS*)aligned_alloc(64,(size_t)CAP*sizeof(MixedAoS));
        uint64_t seed=7777; int alive_count=N;
        for(int i=0;i<CAP;i++){
            e[i].x=(i<N)?(float)(xs64(seed)%(int)WORLD):0;
            e[i].y=(i<N)?(float)(xs64(seed)%(int)WORLD):0;
            e[i].vx=((float)(xs64(seed)%200)-100)*0.05f;
            e[i].vy=((float)(xs64(seed)%200)-100)*0.05f;
            e[i].radius=3.0f+(float)(xs64(seed)%30)*0.1f;
            e[i].type=(int)(xs64(seed)%8);e[i].components=(i<N)?0xFF:0;e[i].alive=(i<N)?1:0;
            e[i].n_refs=(i<N)?(1+(int)(xs64(seed)%4)):0;
            for(int r=0;r<4;r++)e[i].refs[r]=(r<e[i].n_refs)?(int)(xs64(seed)%N):-1;
        }
        
        double total_ms=0; long long total_coll=0; float total_influence=0;
        for(int frame=0;frame<3;frame++){
            double tf=now_ms();
            // 1) Mutation: spawn/despawn 5%
            int to_kill=alive_count/20;
            for(int k=0;k<to_kill;k++){int idx=(int)(xs64(seed)%CAP);if(e[idx].alive){e[idx].alive=0;alive_count--;}}
            for(int s=0;s<to_kill;s++){int idx=(int)(xs64(seed)%CAP);if(!e[idx].alive){e[idx].alive=1;e[idx].x=(float)(xs64(seed)%(int)WORLD);e[idx].y=(float)(xs64(seed)%(int)WORLD);e[idx].components=0x3F;e[idx].n_refs=1+(int)(xs64(seed)%4);alive_count++;}}
            // 2) Toggle components 20%
            for(int i=0;i<CAP;i++){if(!e[i].alive)continue;if(xs64(seed)%5==0)e[i].components^=(1<<(xs64(seed)%8));}
            // 3) Rewire 10%
            for(int i=0;i<CAP;i++){if(!e[i].alive||e[i].n_refs==0)continue;if(xs64(seed)%10==0)e[i].refs[xs64(seed)%e[i].n_refs]=(int)(xs64(seed)%alive_count);}
            // 4) Pointer-chasing influence
            float influence=0;
            for(int i=0;i<CAP;i++){
                if(!e[i].alive||(e[i].components&0x03)==0)continue;
                float val=0;int cur=i;
                for(int depth=0;depth<3;depth++){
                    if(e[cur].n_refs==0)break;int next=e[cur].refs[depth%e[cur].n_refs];
                    if(next<0||next>=CAP||!e[next].alive)break;
                    val+=(e[next].x-e[cur].x)*0.001f+(e[next].y-e[cur].y)*0.001f;cur=next;
                }
                e[i].vx+=val*0.1f;e[i].vy+=val*0.1f;influence+=val;
            }
            total_influence+=influence;
            // 5) Collision
            memset(ccount,0,NCELLS*sizeof(int));
            for(int i=0;i<CAP;i++){if(!e[i].alive){flat[i]=-1;continue;}int gx=max(0,min((int)(e[i].x/CELL),GDIM-1));int gy=max(0,min((int)(e[i].y/CELL),GDIM-1));flat[i]=gy*GDIM+gx;ccount[gy*GDIM+gx]++;}
            cstart[0]=0;for(int i=1;i<NCELLS;i++)cstart[i]=cstart[i-1]+ccount[i-1];memcpy(woff,cstart,NCELLS*sizeof(int));
            for(int i=0;i<CAP;i++){if(flat[i]>=0)sorted_buf[woff[flat[i]]++]=i;}
            int coll=0;static const int d5x[]={0,1,0,1,-1},d5y[]={0,0,1,1,1};
            for(int cy=0;cy<GDIM;cy++)for(int cx=0;cx<GDIM;cx++){
                int ci=cy*GDIM+cx,st=cstart[ci],cnt=ccount[ci];if(!cnt||cnt>100)continue;
                for(int ni=0;ni<5;ni++){int nx=cx+d5x[ni],ny=cy+d5y[ni];if(nx<0||nx>=GDIM||ny<0||ny>=GDIM)continue;
                    int ni2=ny*GDIM+nx,nst=cstart[ni2],nc=ccount[ni2];if(nc>100)continue;
                    for(int ei=st;ei<st+cnt;ei++){int a=sorted_buf[ei];int j0=(ni==0)?ei+1:nst;
                        for(int ej=j0;ej<nst+nc;ej++){int b=sorted_buf[ej];
                            float dx=e[b].x-e[a].x,dy=e[b].y-e[a].y,rs=e[a].radius+e[b].radius,dsq=dx*dx+dy*dy;
                            if(dsq<rs*rs&&dsq>1e-4f){coll++;
                                float d=sqrtf(dsq),nx_c=dx/d,ny_c=dy/d;
                                float dvn=(e[a].vx-e[b].vx)*nx_c+(e[a].vy-e[b].vy)*ny_c;
                                if(dvn>0){e[a].vx-=dvn*nx_c;e[a].vy-=dvn*ny_c;e[b].vx+=dvn*nx_c;e[b].vy+=dvn*ny_c;}
                                float ov=rs-d;if(ov>0){float s=ov*0.5f;e[a].x-=s*nx_c;e[a].y-=s*ny_c;e[b].x+=s*nx_c;e[b].y+=s*ny_c;}
                            }
                        }
                    }
                }
            }
            total_coll+=coll;
            // 6) Movement
            for(int i=0;i<CAP;i++){if(!e[i].alive)continue;e[i].x+=e[i].vx;e[i].y+=e[i].vy;
                if(e[i].x<0){e[i].x=-e[i].x;e[i].vx=-e[i].vx;}if(e[i].y<0){e[i].y=-e[i].y;e[i].vy=-e[i].vy;}
                if(e[i].x>WORLD){e[i].x=2*WORLD-e[i].x;e[i].vx=-e[i].vx;}if(e[i].y>WORLD){e[i].y=2*WORLD-e[i].y;e[i].vy=-e[i].vy;}}
            double frame_ms=now_ms()-tf; total_ms+=frame_ms;
            printf("  AoS Frame %d: %.0f ms | coll=%d | inf=%.0f | alive=%d\n",
                   frame, frame_ms, coll, influence, alive_count);
        }
        record("S6-AoS-TotalFrame", total_ms/3);
        printf("  AoS Mixed:   %.0f ms/frame avg | %lld coll | %zuB/ent\n",
               total_ms/3, total_coll/3, sizeof(MixedAoS));
        free(e);
    }
    
    // ---- Hybrid Mixed ----
    {
        MixedHybrid* hot=(MixedHybrid*)aligned_alloc(64,(size_t)CAP*sizeof(MixedHybrid));
        int* erefs=(int*)aligned_alloc(64,(size_t)CAP*4*sizeof(int));
        int* enrefs=(int*)aligned_alloc(64,(size_t)CAP*sizeof(int));
        float* erad=(float*)aligned_alloc(64,(size_t)CAP*sizeof(float));
        int* ecomp=(int*)aligned_alloc(64,(size_t)CAP*sizeof(int));
        
        uint64_t seed=7777; int alive_count=N;
        for(int i=0;i<CAP;i++){
            hot[i].x=(i<N)?(float)(xs64(seed)%(int)WORLD):0;
            hot[i].y=(i<N)?(float)(xs64(seed)%(int)WORLD):0;
            hot[i].vx=((float)(xs64(seed)%200)-100)*0.05f;
            hot[i].vy=((float)(xs64(seed)%200)-100)*0.05f;
            hot[i].type=(int)(xs64(seed)%8);hot[i].alive=(i<N)?1:0;
            enrefs[i]=(i<N)?(1+(int)(xs64(seed)%4)):0;
            for(int r=0;r<4;r++)erefs[i*4+r]=(r<enrefs[i])?(int)(xs64(seed)%N):-1;
            erad[i]=3.0f+(float)(xs64(seed)%30)*0.1f;
            ecomp[i]=(i<N)?0xFF:0;
        }
        
        double total_ms=0; long long total_coll=0; float total_influence=0;
        for(int frame=0;frame<3;frame++){
            double tf=now_ms();
            int to_kill=alive_count/20;
            for(int k=0;k<to_kill;k++){int idx=(int)(xs64(seed)%CAP);if(hot[idx].alive){hot[idx].alive=0;alive_count--;}}
            for(int s=0;s<to_kill;s++){int idx=(int)(xs64(seed)%CAP);if(!hot[idx].alive){hot[idx].alive=1;hot[idx].x=(float)(xs64(seed)%(int)WORLD);hot[idx].y=(float)(xs64(seed)%(int)WORLD);ecomp[idx]=0x3F;enrefs[idx]=1+(int)(xs64(seed)%4);alive_count++;}}
            for(int i=0;i<CAP;i++){if(!hot[i].alive)continue;if(xs64(seed)%5==0)ecomp[i]^=(1<<(xs64(seed)%8));}
            for(int i=0;i<CAP;i++){if(!hot[i].alive||enrefs[i]==0)continue;if(xs64(seed)%10==0)erefs[i*4+xs64(seed)%enrefs[i]]=(int)(xs64(seed)%alive_count);}
            float influence=0;
            for(int i=0;i<CAP;i++){
                if(!hot[i].alive||(ecomp[i]&0x03)==0)continue;
                float val=0;int cur=i;
                for(int depth=0;depth<3;depth++){
                    if(enrefs[cur]==0)break;int next=erefs[cur*4+depth%enrefs[cur]];
                    if(next<0||next>=CAP||!hot[next].alive)break;
                    val+=(hot[next].x-hot[cur].x)*0.001f+(hot[next].y-hot[cur].y)*0.001f;cur=next;
                }
                hot[i].vx+=val*0.1f;hot[i].vy+=val*0.1f;influence+=val;
            }
            total_influence+=influence;
            memset(ccount,0,NCELLS*sizeof(int));
            for(int i=0;i<CAP;i++){if(!hot[i].alive){flat[i]=-1;continue;}int gx=max(0,min((int)(hot[i].x/CELL),GDIM-1));int gy=max(0,min((int)(hot[i].y/CELL),GDIM-1));flat[i]=gy*GDIM+gx;ccount[gy*GDIM+gx]++;}
            cstart[0]=0;for(int i=1;i<NCELLS;i++)cstart[i]=cstart[i-1]+ccount[i-1];memcpy(woff,cstart,NCELLS*sizeof(int));
            for(int i=0;i<CAP;i++){if(flat[i]>=0)sorted_buf[woff[flat[i]]++]=i;}
            int coll=0;static const int d5x[]={0,1,0,1,-1},d5y[]={0,0,1,1,1};
            for(int cy=0;cy<GDIM;cy++)for(int cx=0;cx<GDIM;cx++){
                int ci=cy*GDIM+cx,st=cstart[ci],cnt=ccount[ci];if(!cnt||cnt>100)continue;
                for(int ni=0;ni<5;ni++){int nx=cx+d5x[ni],ny=cy+d5y[ni];if(nx<0||nx>=GDIM||ny<0||ny>=GDIM)continue;
                    int ni2=ny*GDIM+nx,nst=cstart[ni2],nc=ccount[ni2];if(nc>100)continue;
                    for(int ei=st;ei<st+cnt;ei++){int a=sorted_buf[ei];int j0=(ni==0)?ei+1:nst;
                        for(int ej=j0;ej<nst+nc;ej++){int b=sorted_buf[ej];
                            float dx=hot[b].x-hot[a].x,dy=hot[b].y-hot[a].y,rs=erad[a]+erad[b],dsq=dx*dx+dy*dy;
                            if(dsq<rs*rs&&dsq>1e-4f){coll++;
                                float d=sqrtf(dsq),nx_c=dx/d,ny_c=dy/d;
                                float dvn=(hot[a].vx-hot[b].vx)*nx_c+(hot[a].vy-hot[b].vy)*ny_c;
                                if(dvn>0){hot[a].vx-=dvn*nx_c;hot[a].vy-=dvn*ny_c;hot[b].vx+=dvn*nx_c;hot[b].vy+=dvn*ny_c;}
                                float ov=rs-d;if(ov>0){float s=ov*0.5f;hot[a].x-=s*nx_c;hot[a].y-=s*ny_c;hot[b].x+=s*nx_c;hot[b].y+=s*ny_c;}
                            }
                        }
                    }
                }
            }
            total_coll+=coll;
            for(int i=0;i<CAP;i++){if(!hot[i].alive)continue;hot[i].x+=hot[i].vx;hot[i].y+=hot[i].vy;
                if(hot[i].x<0){hot[i].x=-hot[i].x;hot[i].vx=-hot[i].vx;}if(hot[i].y<0){hot[i].y=-hot[i].y;hot[i].vy=-hot[i].vy;}
                if(hot[i].x>WORLD){hot[i].x=2*WORLD-hot[i].x;hot[i].vx=-hot[i].vx;}if(hot[i].y>WORLD){hot[i].y=2*WORLD-hot[i].y;hot[i].vy=-hot[i].vy;}}
            double frame_ms=now_ms()-tf; total_ms+=frame_ms;
            printf("  Hybrid Frame %d: %.0f ms | coll=%d | inf=%.0f | alive=%d\n",
                   frame, frame_ms, coll, influence, alive_count);
        }
        record("S6-Hybrid-TotalFrame", total_ms/3);
        printf("  Hybrid Mixed: %.0f ms/frame avg | %lld coll | %zuB hot+sep\n",
               total_ms/3, total_coll/3, sizeof(MixedHybrid));
        free(hot);free(erefs);free(enrefs);free(erad);free(ecomp);
    }
    
    free(flat);free(sorted_buf);free(cstart);free(ccount);free(woff);
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    printf("========================================================================\n");
    printf("  ADVERSARIAL BENCHMARK — \"The Compiler vs Reality\"\n");
    printf("  Graph: %d | ECS: %d | Mutation: %d | Collision: %d | Mixed: %d\n",
           N_GRAPH, N_ECS, N_MUT_ECS, N_COLL, N_MIXED);
    printf("  Threads: %d | Machine: 8GB / 4-core Xeon\n", N_THREADS);
    printf("========================================================================\n");
    
    scenario1_graph_traversal();
    scenario2_pointer_chasing();
    scenario3_structural_mutation();
    scenario4_branch_explosion();
    scenario5_atomic_contention();
    scenario6_mixed_mode();
    
    // ========================================================================
    // FINAL SUMMARY
    // ========================================================================
    printf("\n========================================================================\n");
    printf("  FINAL SUMMARY — AoS vs SoA vs Hybrid\n");
    printf("========================================================================\n\n");
    
    printf("  %-40s %10s %10s %10s\n", "Scenario", "AoS (ms)", "SoA (ms)", "Hybrid (ms)");
    printf("  %-40s %10s %10s %10s\n", string(40,'-').c_str(), string(10,'-').c_str(), string(10,'-').c_str(), string(10,'-').c_str());
    
    for (size_t i = 0; i < results.size(); i++) {
        // Group related results
        printf("  %-40s %10.1f\n", results[i].name, results[i].ms);
    }
    
    // Per-scenario comparison table
    printf("\n  Per-Scenario Comparison (3-frame avg):\n\n");
    printf("  %-35s  %8s  %8s  %8s  %s\n", "Scenario", "AoS", "SoA", "Hybrid", "Winner");
    printf("  %-35s  %8s  %8s  %8s  %s\n", string(35,'-').c_str(), string(8,'-').c_str(), string(8,'-').c_str(), string(8,'-').c_str(), string(6,'-').c_str());
    
    // Find matching results for comparison
    auto find_ms = [](const char* prefix) -> double {
        for (auto& r : results) if (strstr(r.name, prefix)) return r.ms;
        return 0;
    };
    
    struct Comparison { const char* label; const char* aos; const char* soa; const char* hyb; };
    Comparison comps[] = {
        {"S1: Graph BFS", "S1-AoS-BFS", "S1-SoA-BFS", "S1-Hybrid-BFS"},
        {"S2: Ptr Chase (resolve)", "S2-AoS-Resolve", "S2-SoA-Resolve", "S2-Hybrid-Resolve"},
        {"S2: Ptr Chase (mutate)", "S2-AoS-Mutate", "S2-SoA-Mutate", "S2-Hybrid-Mutate"},
        {"S3: Struct Mutation", "S3-AoS-Mutation", "S3-SoA-Mutation", "S3-Hybrid-Mutation"},
        {"S4: Branch Explosion (coll)", "S4-AoS-Collision", "", "S4-Hybrid-Collision"},
        {"S5: Atomic (padded)", "S5-Padded-Atomic", "", ""},
        {"S5: Atomic (false share)", "", "S5-Packed-FalseShare", ""},
        {"S6: Mixed Mode /frame", "S6-AoS-TotalFrame", "", "S6-Hybrid-TotalFrame"},
    };
    
    for (auto& c : comps) {
        double a = find_ms(c.aos), s = find_ms(c.soa), h = find_ms(c.hyb);
        double best = 1e18;
        if (a > 0 && a < best) best = a;
        if (s > 0 && s < best) best = s;
        if (h > 0 && h < best) best = h;
        const char* winner = (a > 0 && a == best) ? "AoS" : (s > 0 && s == best) ? "SoA" : (h > 0 && h == best) ? "Hybrid" : "-";
        
        printf("  %-35s  %8s  %8s  %8s  %s\n", c.label,
               a > 0 ? to_string((int)a).c_str() : "-",
               s > 0 ? to_string((int)s).c_str() : "-",
               h > 0 ? to_string((int)h).c_str() : "-",
               winner);
    }
    
    printf("\n  The truth: when computation is truly irregular,\n");
    printf("  no memory layout saves you. The question is:\n");
    printf("  does your compiler degrade gracefully, or does it\n");
    printf("  assume structure that no longer exists?\n\n");
    
    return 0;
}
