//
// Created by isndev on 12/4/18.
//

#ifndef CUBE_CORESET_H
# define CUBE_CORESET_H
# include <cstdint>
# include <vector>
# include <unordered_set>

namespace cube {

    /*!
     * @class CoreSet engine/CoreSet.h cube/coreset.h
     * @ingroup Engine
     * @brief Main initializer
     */
    class CoreSet {
        const std::size_t       _nb_core;
        std::vector<uint8_t>    _set;
        std::size_t             _size;

    public:
        CoreSet() = delete;
        CoreSet(CoreSet const &) = default;
        CoreSet(std::unordered_set<uint8_t> const &set);

        uint8_t resolve(std::size_t const id) const;

        std::size_t getSize() const;
        std::size_t getNbCore() const;
    };

} // namespace cube

#endif //CUBE_CORESET_H
