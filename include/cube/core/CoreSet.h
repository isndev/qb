//
// Created by isndev on 12/4/18.
//

#ifndef QB_CORESET_H
# define QB_CORESET_H
# include <cstdint>
# include <vector>
# include <unordered_set>

namespace qb {

    /*!
     * @class CoreSet core/CoreSet.h cube/coreset.h
     * @ingroup Engine
     * @brief Main initializer
     */
    class CoreSet {
        friend class Main;

        const std::unordered_set<uint8_t>  _raw_set;
        const std::size_t       _nb_core;
        std::vector<uint8_t>    _set;
        std::size_t             _size;

        uint8_t resolve(std::size_t const id) const;
        std::size_t getSize() const;
        std::size_t getNbCore() const;

    public:
        CoreSet() = delete;
        CoreSet(CoreSet const &) = default;
        explicit CoreSet(std::unordered_set<uint8_t> const &set);

        static CoreSet build(uint32_t const nb_core = std::thread::hardware_concurrency());
    };

} // namespace qb

#endif //QB_CORESET_H
