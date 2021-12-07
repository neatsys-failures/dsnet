#include "replication/minbft/adapter.h"

namespace dsnet {
namespace minbft {

opnum_t MinBFTPlainAdapter::s_ui = 0;

MinBFTAdapter::MinBFTAdapter(
    Message *inner, const std::string &identifier, bool assign_ui)
    : plain_layer(inner, assign_ui), signed_layer(plain_layer, identifier) {}

} // namespace minbft
} // namespace dsnet