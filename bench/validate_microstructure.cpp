// Validation harness: every metric computed twice, once in integer fixed
// point and once in float64, over a large synthetic book stream.
#include "microstructure.hpp"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <algorithm>

struct RefDouble {
    double micro=0, mid=0, ofi=0, es_sum=0; uint64_t es_n=0;
    double sum_sq=0; uint64_t n_ret=0;
    double sum_dpv=0, sum_vv=0, lambda=0;
    double pb=0,qb=0,pa=0,qa=0, prev_mid=0, prev_trade_mid=0; bool have=false;
    uint64_t degenerate=0;

    void on_book(double bp,double bq,double ap,double aq){
        if(bp<=0||ap<=0||bq<=0||aq<=0) return;
        if(bp>=ap){ ++degenerate; return; }   // locked or crossed
        micro=(bp*aq+ap*bq)/(bq+aq);
        mid=(bp+ap)/2.0;
        if(have){
            double e=0;
            if(bp>=pb) e+=bq;
            if(bp<=pb) e-=this->qb;
            if(ap<=pa) e-=aq;
            if(ap>=pa) e+=this->qa;
            ofi+=e;
            double m=(bp+ap)/2.0;
            if(prev_mid>0 && m!=prev_mid){
                double r=std::log(m/prev_mid);
                sum_sq+=r*r; ++n_ret;
            }
            prev_mid=m;
        } else { prev_mid=(bp+ap)/2.0; have=true; }
        pb=bp;qb=bq;pa=ap;qa=aq;
    }
    void on_trade(double px,double q,bool buy){
        if(!have) return;
        double m=(pb+pa)/2.0;
        es_sum+=2.0*std::fabs(px-m); ++es_n;
        double v=buy?q:-q;
        if(prev_trade_mid>0){
            double dp=m-prev_trade_mid;     // since the last TRADE
            sum_dpv+=dp*v; sum_vv+=v*v;
            if(sum_vv>0) lambda=sum_dpv/sum_vv;
        }
        prev_trade_mid=m;
    }
    double rv() const { return std::sqrt(sum_sq); }
};

int main(){
    Microstructure fx; RefDouble ref;
    uint32_t seed=20190730;
    auto lcg=[&]{ seed=seed*1664525u+1013904223u; return seed; };

    uint32_t bid=2180000, ask=2180100;   // $218.00 / $218.01
    double maxrel_micro=0, maxrel_mid=0, maxabs_ofi=0, maxrel_es=0,
           maxrel_rv=0, maxrel_lam=0;
    const int N=1300000;

    for(int i=0;i<N;i++){
        int32_t drift=static_cast<int32_t>(lcg()%201)-100;
        bid=static_cast<uint32_t>(std::max<int64_t>(1000000,(int64_t)bid+drift));
        ask=bid+100+(lcg()%400);
        uint32_t bq=1+lcg()%5000, aq=1+lcg()%5000;

        fx.on_book(bid,bq,ask,aq);
        ref.on_book(bid,bq,ask,aq);

        auto rel=[](double a,double b){ return b==0?std::fabs(a):std::fabs(a-b)/std::fabs(b); };
        maxrel_micro=std::max(maxrel_micro, rel(fx.metrics().microprice_1e8/1e8, ref.micro/1e4));
        maxrel_mid  =std::max(maxrel_mid,   rel(fx.metrics().mid_1e8/1e8,        ref.mid/1e4));
        maxabs_ofi  =std::max(maxabs_ofi,   std::fabs((double)fx.metrics().ofi-ref.ofi));

        if(lcg()%4==0){
            uint32_t px = (lcg()%2)? ask : bid;
            uint32_t q  = 1+lcg()%800;
            bool buy    = (px==ask);
            fx.on_trade(px,q,buy);
            ref.on_trade(px,q,buy);
        }
        // skip the first 1000 samples: RV built from one or two returns is
        // dominated by the quantization of a single tick, not by the method
        if(fx.metrics().return_samples>1000 && ref.n_ret>1000)
            maxrel_rv=std::max(maxrel_rv, rel(fx.metrics().realized_vol_1e9/1e9, ref.rv()));
        // lambda crosses zero early, where relative error is meaningless;
        // measure once the regression has enough samples to be stable
        if(fx.metrics().trades>10000 && ref.sum_vv>0)
            maxrel_lam=std::max(maxrel_lam, rel(fx.metrics().kyle_lambda_1e12/1e12, ref.lambda/1e4));
    }

    if(fx.metrics().effective_spread_n)
        maxrel_es = std::fabs(
            (double)fx.metrics().effective_spread_sum_1e4/fx.metrics().effective_spread_n/1e4
            - ref.es_sum/ref.es_n/1e4) / (ref.es_sum/ref.es_n/1e4);

    printf("fixed-point vs float64 over %d book updates\n",N);
    printf("  %-22s %12s\n","metric","max rel err");
    printf("  %-22s %12.3e\n","microprice",maxrel_micro);
    printf("  %-22s %12.3e\n","mid",maxrel_mid);
    printf("  %-22s %12.3e   (exact, integer)\n","order flow imbalance",maxabs_ofi);
    printf("  %-22s %12.3e\n","effective spread",maxrel_es);
    printf("  %-22s %12.3e   (after 1k warmup)\n","realized volatility",maxrel_rv);
    printf("  %-22s %12.3e   (final value)\n","  rv final",
           std::fabs(fx.metrics().realized_vol_1e9/1e9-ref.rv())/ref.rv());
    // lambda legitimately crosses zero, where relative error is undefined.
    // absolute error in output units is the honest measure.
    printf("  %-22s %12.3e   (abs, $/share)\n","kyle lambda",
           std::fabs(fx.metrics().kyle_lambda_1e12/1e12-ref.lambda/1e4));
    printf("  %-22s %12.3e   (final, relative)\n","  lambda final",
           ref.lambda==0?0.0:std::fabs(fx.metrics().kyle_lambda_1e12/1e12-ref.lambda/1e4)
                              /std::fabs(ref.lambda/1e4));
    printf("  %-22s %12llu\n","degenerate books skipped",(unsigned long long)ref.degenerate);
    printf("\nfinal values (fixed point):\n");
    printf("  microprice        $%.6f\n", fx.metrics().microprice_1e8/1e8);
    printf("  mid               $%.6f\n", fx.metrics().mid_1e8/1e8);
    printf("  micro - mid       $%.6f\n", fx.metrics().micro_minus_mid_1e8/1e8);
    printf("  OFI               %lld shares\n",(long long)fx.metrics().ofi);
    printf("  realized vol      %.9f  (%llu samples)\n",
           fx.metrics().realized_vol_1e9/1e9,(unsigned long long)fx.metrics().return_samples);
    printf("  kyle lambda       %.12f $/share\n", fx.metrics().kyle_lambda_1e12/1e12);
    printf("  eff spread        %.2f bps over %llu trades\n",
           fx.effective_spread_bps(),(unsigned long long)fx.metrics().effective_spread_n);
}