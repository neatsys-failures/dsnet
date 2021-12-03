#pragma once
#include "lib/transport.h"

namespace dsnet {
namespace minbft {

class MinBFTAdapter : public Message {
public:
    // if want to serialize, must instantiate in replica thread
    // MinBFTAdapter(bool )
};

}
}