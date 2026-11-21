#include <stdexcept>

#include "crdt/algorithms/logoot.hpp"
#include "crdt/algorithms/lseq.hpp"
#include "crdt/algorithms/rga.hpp"
#include "crdt/algorithms/treedoc.hpp"
#include "crdt/algorithms/woot.hpp"

namespace crdt {

std::unique_ptr<SequenceCRDT> make_crdt(Algorithm algorithm, ReplicaId site, const CRDTOptions& options) {
    switch (algorithm) {
        case Algorithm::WOOT: return std::make_unique<Woot>(site);
        case Algorithm::RGA: return std::make_unique<Rga>(site);

        case Algorithm::Logoot: {
            LogootRules rules;
            rules.boundary = options.logoot_boundary;
            return std::make_unique<Logoot>(site, rules, options.seed);
        }

        case Algorithm::LSEQ: {
            LseqRules rules;
            rules.base_bits = options.lseq_base_bits;
            rules.boundary = options.lseq_boundary;
            rules.strategy_seed = options.seed;
            return std::make_unique<LSEQ>(site, rules, options.seed);
        }

        case Algorithm::Treedoc: return std::make_unique<Treedoc>(site, options.treedoc_collect_garbage);
    }
    throw std::invalid_argument("unknown algorithm");
}

}  // namespace crdt
