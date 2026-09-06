#pragma once

#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Strata {

enum class Placement : std::uint8_t {
	Default,
	Internal,
	PreferExternal,
	RequireExternal,
};

enum class Region : std::uint8_t {
	Unknown,
	Internal,
	External,
};

constexpr bool validPlacement(Placement placement) noexcept {
	switch (placement) {
	case Placement::Default:
	case Placement::Internal:
	case Placement::PreferExternal:
	case Placement::RequireExternal: return true;
	}
	return false;
}

constexpr const char *toString(Placement placement) noexcept {
	switch (placement) {
	case Placement::Default: return "default";
	case Placement::Internal: return "internal";
	case Placement::PreferExternal: return "prefer-external";
	case Placement::RequireExternal: return "require-external";
	}
	return "unknown";
}

constexpr const char *toString(Region region) noexcept {
	switch (region) {
	case Region::Unknown: return "unknown";
	case Region::Internal: return "internal";
	case Region::External: return "external";
	}
	return "unknown";
}

struct MemoryPolicy {
	Placement allocation{Placement::Default};
	Placement taskStack{Placement::Internal};
};

constexpr bool validMemoryPolicy(const MemoryPolicy &policy) noexcept {
	return validPlacement(policy.allocation) && validPlacement(policy.taskStack);
}

template <typename T>
class Allocator {
  public:
	using value_type = T;

	explicit Allocator(Placement placement = Placement::Default) noexcept : _placement(placement) {}

	template <typename U>
	Allocator(const Allocator<U> &other) noexcept : _placement(other.placement()) {}

	T *allocate(std::size_t count) {
		return std::allocator<T>{}.allocate(count);
	}

	void deallocate(T *ptr, std::size_t count) noexcept {
		std::allocator<T>{}.deallocate(ptr, count);
	}

	Placement placement() const noexcept { return _placement; }

	template <typename U>
	struct rebind { using other = Allocator<U>; };

  private:
	Placement _placement;
};

template <typename T, typename U>
constexpr bool operator==(const Allocator<T> &lhs, const Allocator<U> &rhs) noexcept {
	return lhs.placement() == rhs.placement();
}

template <typename T, typename U>
constexpr bool operator!=(const Allocator<T> &lhs, const Allocator<U> &rhs) noexcept {
	return !(lhs == rhs);
}

template <typename T>
using Vector = std::vector<T, Allocator<T>>;

using String = std::basic_string<char, std::char_traits<char>, Allocator<char>>;

template <typename T>
using UniquePtr = std::unique_ptr<T>;

template <typename T, typename... Args>
UniquePtr<T> makeUnique(Placement, Args &&...args) noexcept {
	return UniquePtr<T>{new (std::nothrow) T(std::forward<Args>(args)...)};
}

template <typename T, typename... Args>
std::shared_ptr<T> makeShared(Placement placement, Args &&...args) {
	return std::allocate_shared<T>(Allocator<T>{placement}, std::forward<Args>(args)...);
}

} // namespace Strata
