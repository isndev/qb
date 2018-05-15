#include "system/actor/IActor.h"
#include "system/actor/Types.h"

const cube::ActorId cube::ActorId::NotFound = {0, 0};
std::mutex cube::io::io_lock;

// debug stuff
std::ostream &operator<<(std::ostream &os, cube::ActorId const &id) {
    os << "ActorID[id(" << id._id << ") idx(" << id._index << ") id_64(" << static_cast<uint64_t>(id) << ")]";
    return os;
}