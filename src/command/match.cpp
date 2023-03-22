#include <command/match.hpp>
#include <command/commands.hpp>
#include <threadContext.hpp>
#include <ds.hpp>
#include <swapchain.hpp>
#include <lmm.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <util/dlg.hpp>
#include <util/profiling.hpp>

// We interpret matching of two command sequences submitted
// to the gpu as an instance of the common longest subsequence
// problem (LCS). We solve it via the custom lazy-matrix-march
// algorithm (LMM), see lmm.hpp.
// In our case, we want to match hierarchies and therefore
// have recursive instances of LMM.

namespace vil {

// TODO: move somewhere else. In commands.cpp atm
bool same(const Pipeline* a, const Pipeline* b);

float eval(const Matcher& m) {
	dlg_assertm(valid(m), "match {}, total {}", m.match, m.total);
	if(m.match == 0.f) { // no match
		// For match=total=0, we return 1.f by design.
		// This means we had no data to match on.
		// Imagine an empty span, we want to consider two of them similar.
		return m.total == 0.f ? 1.f : 0.f;
	}

	return m.match / m.total;
}

bool valid(const Matcher& m) {
	return m.total == -1.f || m.match <= m.total;
}

bool addNonEmpty(Matcher& m, std::string_view str1, std::string_view str2, float w) {
	if(str1.empty() && str2.empty()) {
		return true;
	}

	return add(m, str1, str2, w);
}

// TODO: code duplication with commands.cpp, but moving it to match.hpp
// instead of having it twice causes weird issues with overloading and name
// lookup
template<typename T>
void addSpanOrderedStrict(Matcher& m, span<T> a, span<T> b, float weight = 1.0) {
	m.total += weight;

	if(a.size() != b.size()) {
		return;
	}

	if(a.empty()) {
		m.match += weight;
		return;
	}

	Matcher accum {};
	for(auto i = 0u; i < a.size(); ++i) {
		auto res = match(a[i], b[i]);
		accum.match += res.match;
		accum.total += res.total;
	}

	// TODO: maybe better to make weight also dependent on size?
	// could add flag/param for that behavior.
	m.match += weight * eval(accum);
}

Matcher match(const DescriptorSetLayout::Binding& a,
		const DescriptorSetLayout::Binding& b) {
	Matcher m;
	if(!add(m, a.descriptorType, b.descriptorType)) {
		return m;
	}

	add(m, a.descriptorCount, b.descriptorCount);
	add(m, a.flags, b.flags);
	add(m, a.stageFlags, b.stageFlags);
	add(m, a.immutableSamplers ? true : false,
		b.immutableSamplers ? true : false);
	return m;
}

void add(Matcher& m, const DescriptorSetLayout& a, const DescriptorSetLayout& b) {
	addNonEmpty(m, a.name, b.name, 20.f);
	add(m, a.flags, b.flags);
	addSpanOrderedStrict(m,
		span<const DescriptorSetLayout::Binding>(a.bindings),
		span<const DescriptorSetLayout::Binding>(b.bindings),
		float(std::max(a.bindings.size(), b.bindings.size())));
}

void add(Matcher& m, const Image& a, const Image& b) {
	addNonEmpty(m, a.name, b.name, 10.f);
	add(m, a.ci.extent.width, b.ci.extent.width);
	add(m, a.ci.extent.height, b.ci.extent.height);
	add(m, a.ci.extent.depth, b.ci.extent.depth);
	add(m, a.ci.arrayLayers, b.ci.arrayLayers);
	add(m, a.ci.mipLevels, b.ci.mipLevels);
	add(m, a.ci.imageType, b.ci.imageType);
	add(m, a.ci.tiling, b.ci.tiling);
	add(m, a.ci.sharingMode, b.ci.sharingMode);
	add(m, a.ci.samples, b.ci.samples);
	add(m, a.ci.format, b.ci.format);
	add(m, a.ci.flags, b.ci.flags);
	add(m, a.ci.usage, b.ci.usage);
}

bool addImgOrSwapchain(Matcher& m, const ImageView* va, const ImageView* vb) {
	bool same = false;
	if(va && vb && va != vb && va->img && vb->img && va->img->swapchain) {
		same = (va->img->swapchain == vb->img->swapchain);
	} else {
		// the image views have to match, not the images to account
		// for different mips or layers
		// TODO: could consider the imageView description here instead?
		// But creating similar image views for the same image is a weird corner case.
		same = (va == vb);
	}

	// TODO WIP, for serialize-matching
	constexpr bool deepMatching = false;
	if constexpr(deepMatching && !same) {
		if(va->img == vb->img) {
			add(m, true, true, 30.f);
			return true;
		}

		if(va->img && vb->img) {
			add(m, *va->img, *vb->img);
		}

		// always succeed.
		return true;
	}

	add(m, same, true, 30.f);
	return same;
}

void add(Matcher& m, const VkImageSubresourceRange& a, const VkImageSubresourceRange& b,
		float weight) {
	add(m, a.aspectMask, b.aspectMask, weight);
	add(m, a.baseArrayLayer, b.baseArrayLayer, weight);
	add(m, a.baseMipLevel, b.baseMipLevel, weight);
	add(m, a.levelCount, b.levelCount, weight);
	add(m, a.layerCount, b.layerCount, weight);
}

void add(Matcher& m, const VkImageSubresourceLayers& a, const VkImageSubresourceLayers& b,
		float weight) {
	add(m, a.aspectMask, b.aspectMask, weight);
	add(m, a.baseArrayLayer, b.baseArrayLayer, weight);
	add(m, a.mipLevel, b.mipLevel, weight);
	add(m, a.layerCount, b.layerCount, weight);
}

Matcher match(const ImageView& a, const ImageView& b) {
	Matcher m;

	// we require that they point to the same underlying resource
	if(!addImgOrSwapchain(m, &a, &b)) {
		return m;
	}

	addNonEmpty(m, a.name, b.name, 20.f);
	add(m, a.ci.components.a, b.ci.components.a, 0.25f);
	add(m, a.ci.components.r, b.ci.components.r, 0.25f);
	add(m, a.ci.components.g, b.ci.components.g, 0.25f);
	add(m, a.ci.components.b, b.ci.components.b, 0.25f);
	add(m, a.ci.format, b.ci.format);
	add(m, a.ci.viewType, b.ci.viewType);
	add(m, a.ci.subresourceRange, b.ci.subresourceRange);

	return m;
}

Matcher match(const BufferView& a, const BufferView& b) {
	Matcher m;

	// we require that they point to the same underlying resource
	// TODO: deep match
	if(!add(m, a.buffer, b.buffer)) {
		return m;
	}

	addNonEmpty(m, a.name, b.name, 20.f);
	add(m, a.ci.format, b.ci.format);
	add(m, a.ci.offset, b.ci.offset);
	add(m, a.ci.range, b.ci.range);
	add(m, a.ci.flags, b.ci.flags);

	return m;
}

Matcher match(const Sampler& a, const Sampler& b) {
	Matcher m;
	add(m, a.ci.addressModeU, b.ci.addressModeU);
	add(m, a.ci.addressModeV, b.ci.addressModeV);
	add(m, a.ci.addressModeW, b.ci.addressModeW);
	add(m, a.ci.mipmapMode, b.ci.mipmapMode);
	add(m, a.ci.borderColor, b.ci.borderColor);
	add(m, a.ci.magFilter, b.ci.magFilter);
	add(m, a.ci.minFilter, b.ci.minFilter);
	add(m, a.ci.maxLod, b.ci.maxLod);
	add(m, a.ci.minLod, b.ci.minLod);
	add(m, a.ci.mipLodBias, b.ci.mipLodBias);
	add(m, a.ci.unnormalizedCoordinates, b.ci.unnormalizedCoordinates);

	add(m, a.ci.compareEnable, b.ci.compareEnable);
	if(a.ci.compareEnable && b.ci.compareEnable) {
		add(m, a.ci.compareOp, b.ci.compareOp);
	}

	add(m, a.ci.anisotropyEnable, b.ci.anisotropyEnable);
	if(a.ci.anisotropyEnable && b.ci.anisotropyEnable) {
		add(m, a.ci.maxAnisotropy, b.ci.maxAnisotropy);
	}

	return m;
}

Matcher match(const DescriptorStateRef& a, const DescriptorStateRef& b) {
	dlg_assert(a.layout);
	dlg_assert(b.layout);

	// we expect them to have the same layout since they must
	// be bound for commands with the same pipeline
	// TODO: not strictly required anymore, we e.g. want to support pipe
	//  reload. In that case we could still return a valid result here even
	//  if some bindings vanished or new appeared.
	dlg_assert_or(compatible(*a.layout, *b.layout), return Matcher::noMatch());

	// fast path: full match since same descriptorSet
	if(a.data == b.data) {
		auto count = float(totalDescriptorCount(a));
		return Matcher{count, count};
	}

	// iterate over bindings
	Matcher m;
	for(auto bindingID = 0u; bindingID < a.layout->bindings.size(); ++bindingID) {
		// they can have different size, when variable descriptor count is used
		auto sizeA = descriptorCount(a, bindingID);
		auto sizeB = descriptorCount(b, bindingID);

		// must have the same type
		auto dsType = a.layout->bindings[bindingID].descriptorType;
		dlg_assert_or(a.layout->bindings[bindingID].descriptorType ==
			b.layout->bindings[bindingID].descriptorType, continue);

		// hole in the layout
		if(dsType == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
			dlg_assert(sizeA == 0u && sizeB == 0u);
			continue;
		}

		if(dsType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
			// This might seem like a low weight but the bytewise
			// comparison isn't the best anyways. Counting the number
			// of equal bytes or weighing this by the block size
			// would be bad.
			m.total += 1;
		} else {
			// sizeA might not be the same as sizeB for variable descriptor counts
			m.total += std::max(sizeA, sizeB);
		}

		// if samplers or image/buffers views are different we check them for
		// semantic equality as well. Applications sometimes create
		// them lazily/on-demand or stuff like that.

		auto dsCat = vil::category(dsType);
		if(dsCat == DescriptorCategory::image) {
			auto bindingsA = images(a, bindingID);
			auto bindingsB = images(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto& bindA = bindingsA[e];
				auto& bindB = bindingsB[e];

				Matcher combined;

				if(needsSampler(dsType)) {
					combined.total += 1;
					if(bindA.sampler == bindB.sampler) {
						combined.match += 1;
					} else {
						Matcher tmp;
						add(tmp, bindA.sampler, bindB.sampler);
						combined.match += eval(tmp);
					}
				}

				if(needsImageView(dsType)) {
					combined.total += 1;
					if(bindA.imageView == bindB.imageView) {
						combined.match += 1;
					} else {
						Matcher tmp;
						add(tmp, bindA.imageView, bindB.imageView);
						combined.match += eval(tmp);
					}
				}

				// NOTE: consider image layout? not too relevant I guess
				m.match += eval(combined);
			}
		} else if(dsCat == DescriptorCategory::buffer) {
			auto bindingsA = buffers(a, bindingID);
			auto bindingsB = buffers(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto& bindA = bindingsA[e];
				auto& bindB = bindingsB[e];
				// NOTE: consider offset? not too relevant I guess
				if(bindA.buffer == bindB.buffer &&
						bindA.range == bindB.range) {
					++m.match;
				}
			}
		} else if(dsCat == DescriptorCategory::bufferView) {
			auto bindingsA = bufferViews(a, bindingID);
			auto bindingsB = bufferViews(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				if(bindingsA[e] == bindingsB[e]) {
					++m.match;
					continue;
				}

				// test for equality
				Matcher tmp;
				add(tmp, bindingsA[e], bindingsB[e]);
				m.match += eval(tmp);
			}
		} else if(dsCat == DescriptorCategory::accelStruct) {
			auto bindingsA = accelStructs(a, bindingID);
			auto bindingsB = accelStructs(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				if(bindingsA[e] == bindingsB[e]) {
					++m.match;
				}
			}
		} else if(dsCat == DescriptorCategory::inlineUniformBlock) {
			auto bytesA = inlineUniformBlock(a, bindingID);
			auto bytesB = inlineUniformBlock(b, bindingID);
			if(bytesA.size() == bytesB.size() &&
					std::memcmp(bytesA.data(), bytesB.data(), bytesA.size()) == 0) {
				++m.match;
			}
		} else {
			dlg_error("Unsupported descriptor type: {}", u32(dsType));
		}
	}

	return m;
}

bool same(span<void*> a, span<void*> b, unsigned offset = 1u) {
	if(a.size() != b.size()) {
		return false;
	}

	if(a.size() <= offset) {
		return true;
	}

	return std::equal(a.begin() + offset, a.end(), b.begin() + offset);
}

// Adds the given stats to the given matcher
void add(Matcher& m, const ParentCommand::SectionStats& a, const ParentCommand::SectionStats& b) {
	auto addMatch = [&](u32 dst, u32 src, float weight = 1.f) {
		m.match += weight * std::min(dst, src); // in range [0, max]
		m.total += weight * std::max(dst, src);
	};

	addMatch(a.numDispatches, b.numDispatches);
	addMatch(a.numDraws, b.numDraws);
	addMatch(a.numRayTraces, b.numRayTraces);
	addMatch(a.numTransfers, b.numTransfers);
	addMatch(a.numSyncCommands, b.numSyncCommands);

	addMatch(a.numTotalCommands, b.numTotalCommands);

	// every pipeline match counts like N commands
	const auto pipeWeight = 10.f;
	m.total += pipeWeight * std::max(a.numPipeBinds, b.numPipeBinds);

	// TODO: slightly asymmetrical in special cases. Problem?
	for(auto pipeA = b.boundPipelines; pipeA; pipeA = pipeA->next) {
		for(auto pipeB = b.boundPipelines; pipeB; pipeB = pipeB->next) {
			if(same(pipeA->pipe, pipeB->pipe)) {
				m.match += pipeWeight;
				break;
			}
		}
	}
}

float approxTotalWeight(const ParentCommand& cmd) {
	float ret = 10.f; // wild guess
	ret += cmd.sectionStats().numPipeBinds * 10.f;
	for(auto it = cmd.firstChildParent(); it; it = it->nextParent_) {
		ret += approxTotalWeight(*it);
	}
	return ret;
}

float approxTotalWeight(const FrameSubmission& subm) {
	auto ret = 1.f; // queue
	for(auto& sub : subm.submissions) {
		ret += approxTotalWeight(*sub->commands);
	}
	return ret;
}

LazyMatrixMarch::Result runLMM(u32 width, u32 height,
		LinAllocScope& localMem, LazyMatrixMarch::Matcher matcher) {
	constexpr auto branchThreshold = 0.9f;
	auto customUse = localMem.customUse();
	LazyMatrixMarch lmm(width, height, localMem.tc, matcher, branchThreshold);
	return lmm.run();
}

// command hierarchy matching
CommandSectionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
		const ParentCommand& rootA, const ParentCommand& rootB) {
	ZoneScoped;

	// TODO: fast patch for &rootA == &rootB
	// take additional parameter on whether the matches are really needed?
	// in some cases they are not, which would make this a lot cheaper.

	CommandSectionMatch ret;
	ret.a = &rootA;
	ret.b = &rootB;

	// match commands themselves
	ret.match = rootA.match(rootB);
	dlg_assert(valid(ret.match));
	if(ret.match.match <= 0.f) {
		return ret;
	}

#ifdef VIL_COMMAND_CALLSTACKS
	// NOTE WIP: use callstacks for matching.
	// Really hard-reject if they aren't the same?
	// Should probably make this an option, there might be
	// special cases I'm not thinkin of rn.
	if(!same(rootA.stacktrace, rootB.stacktrace)) {
		ret.match = Matcher::noMatch();
		return ret;
	}
#endif // VIL_COMMAND_CALLSTACKS

	// consider sectionStats for rootMatch.
	auto statsA = rootA.sectionStats();
	auto statsB = rootB.sectionStats();
	add(ret.match, statsA, statsB);
	dlg_assert(valid(ret.match));

	auto numSectionsA = rootA.sectionStats().numChildSections;
	auto numSectionsB = rootB.sectionStats().numChildSections;

	// no child matching to do
	// but make sure to account for the "lost" match values here as well
	if(numSectionsA == 0u || numSectionsB == 0u) {
		for(auto it = rootA.firstChildParent(); it; it = it->nextParent_) {
			ret.match.total += approxTotalWeight(*it);
		}

		for(auto it = rootB.firstChildParent(); it; it = it->nextParent_) {
			ret.match.total += approxTotalWeight(*it);
		}

		return ret;
	}

	// store sections for fast random access below
	auto sectionsA = localMem.alloc<const ParentCommand*>(numSectionsA);
	auto id = 0u;
	for(auto it = rootA.firstChildParent(); it; it = it->nextParent_) {
		sectionsA[id] = it;
		++id;
	}

	auto sectionsB = localMem.alloc<const ParentCommand*>(numSectionsB);
	id = 0u;
	for(auto it = rootB.firstChildParent(); it; it = it->nextParent_) {
		sectionsB[id] = it;
		++id;
	}

	// the resulting matches, filled lazily
	auto evalMatches = localMem.alloc<CommandSectionMatch>(numSectionsA * numSectionsB);

	// our matcher as passed to the matching algorithm below
	auto matchingFunc = [&](u32 i, u32 j) {
		ExtZoneScoped;

		auto& parentA = *sectionsA[i];
		auto& parentB = *sectionsB[j];

		auto& dst = evalMatches[j * numSectionsA + i];
		dlg_assert(!dst.a);

		// make sure that we can re-use the local memory after this
		LinAllocScope localNext(localMem.tc);
		dst = match(retMem, localNext, parentA, parentB);
		return eval(dst.match);

		// NOTE: alternative evaluation
		// Might seem weird at first, but here we'd be maximizing
		// the absolute number of successful matches instead of the rate, which
		// also makes sense.
		// Natively, evaluating the rate only would mean we might prefer
		// a small section with 0.7 match over a gian section with 0.6
		// match which might not be expected.
		// But since we evaluate the "missed weight" below as well,
		// the rate-based-evaluation we do above makes more sense
		// return dst.match.match;
	};

	auto lmmRes = runLMM(numSectionsA, numSectionsB, localMem, matchingFunc);

	ret.children = retMem.alloc<CommandSectionMatch>(lmmRes.matches.size());
	id = 0u;
	auto nextI = 0u;
	auto nextJ = 0u;
	for(auto& match : lmmRes.matches) {
		auto& src = evalMatches[match.j * numSectionsA + match.i];
		dlg_assert(src.a);

		ret.children[id] = src;
		ret.match.match += src.match.match;
		ret.match.total += src.match.total;

		// add approximation of missed weight to ret.match.total
		for(; nextI < match.i; ++nextI) {
			ret.match.total += approxTotalWeight(*sectionsA[nextI]);
		}

		for(; nextJ < match.j; ++nextJ) {
			ret.match.total += approxTotalWeight(*sectionsB[nextJ]);
		}

		++nextI;
		++nextJ;
		++id;
	}

	for(; nextI < numSectionsA; ++nextI) {
		ret.match.total += approxTotalWeight(*sectionsA[nextI]);
	}

	for(; nextJ < numSectionsB; ++nextJ) {
		ret.match.total += approxTotalWeight(*sectionsB[nextJ]);
	}

	return ret;
}

FrameSubmissionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
		const FrameSubmission& a, const FrameSubmission& b) {
	// TODO WIP: nullptr queue for serialize
	if(a.queue != b.queue && a.queue && b.queue) {
		return {Matcher::noMatch(), &a, &b, {}};
	}

	FrameSubmissionMatch ret;
	ret.a = &a;
	ret.b = &b;
	// for matching queue
	ret.match.match += 1.f;
	ret.match.total += 1.f;

	// NOTE: add matching for sparse bindings?
	//   This probably does not make sense though since no application
	//   will submit similar sparse bindings over and over again so matching
	//   is somewhat pointless

	if(a.submissions.size() == 0u && b.submissions.size() == 0u) {
		return ret;
	} else if(a.submissions.size() == 0u || b.submissions.size() == 0u) {
		for(auto& subm : a.submissions) {
			ret.match.total += approxTotalWeight(*subm->commands);
		}
		for(auto& subm : b.submissions) {
			ret.match.total += approxTotalWeight(*subm->commands);
		}
		return ret;
	}

	// the resulting matches, filled lazily
	auto evalMatches = localMem.alloc<CommandRecordMatch>(
		a.submissions.size() * b.submissions.size());

	auto matchingFunc = [&](u32 i, u32 j) {
		LinAllocScope nextLocalMem(localMem.tc);

		// TODO: consider name and other properties of the records?

		auto& recA = *a.submissions[i];
		auto& recB = *b.submissions[j];
		auto ret = match(retMem, nextLocalMem,
			*recA.commands, *recB.commands);

		evalMatches[j * a.submissions.size() + i].a = &recA;
		evalMatches[j * a.submissions.size() + i].b = &recB;
		evalMatches[j * a.submissions.size() + i].matches = retMem.alloc<CommandSectionMatch>(1u);
		evalMatches[j * a.submissions.size() + i].matches[0] = ret;
		evalMatches[j * a.submissions.size() + i].match = ret.match;

		return eval(ret.match);
	};

	auto lmmRes = runLMM(a.submissions.size(), b.submissions.size(),
		localMem, matchingFunc);

	// fill ret
	ret.matches = retMem.alloc<CommandRecordMatch>(lmmRes.matches.size());
	auto id = 0u;
	auto nextI = 0u;
	auto nextJ = 0u;
	for(auto& match : lmmRes.matches) {
		auto& src = evalMatches[match.j * a.submissions.size() + match.i];
		ret.matches[id] = src;
		ret.match.match += src.match.match;
		ret.match.total += src.match.total;

		// add approximation of missed weight to ret.match.total
		for(; nextI < match.i; ++nextI) {
			ret.match.total += approxTotalWeight(*a.submissions[nextI]->commands);
		}

		for(; nextJ < match.j; ++nextJ) {
			ret.match.total += approxTotalWeight(*b.submissions[nextJ]->commands);
		}

		++nextI;
		++nextJ;
		++id;
	}

	// add approximation of missed weight to ret.match.total
	for(; nextI < a.submissions.size(); ++nextI) {
		ret.match.total += approxTotalWeight(*a.submissions[nextI]->commands);
	}

	for(; nextJ < b.submissions.size(); ++nextJ) {
		ret.match.total += approxTotalWeight(*b.submissions[nextJ]->commands);
	}

	return ret;
}

FrameMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
		span<const FrameSubmission> a, span<const FrameSubmission> b) {
	ZoneScoped;

	if(a.empty() && b.empty()) {
		// empty actually means full match
		return {};
	} else if(a.empty() || b.empty()) {
		return {Matcher::noMatch(), {}};
	}

	// the resulting matches, filled lazily
	auto evalMatches = localMem.alloc<FrameSubmissionMatch>(
		a.size() * b.size());

	auto matchingFunc = [&](u32 i, u32 j) {
		LinAllocScope nextLocalMem(localMem.tc);
		auto ret = match(retMem, nextLocalMem, a[i], b[j]);
		evalMatches[j * a.size() + i] = ret;
		return eval(ret.match);
	};

	auto lmmRes = runLMM(a.size(), b.size(), localMem, matchingFunc);

	// fill return info
	FrameMatch ret;
	ret.matches = retMem.alloc<FrameSubmissionMatch>(lmmRes.matches.size());
	auto id = 0u;
	auto nextI = 0u;
	auto nextJ = 0u;
	for(auto& match : lmmRes.matches) {
		auto& src = evalMatches[match.j * a.size() + match.i];
		ret.matches[id] = src;
		ret.match.match += src.match.match;
		ret.match.total += src.match.total;

		// add approximation of missed weight to ret.match.total
		for(; nextI < match.i; ++nextI) {
			ret.match.total += approxTotalWeight(a[nextI]);
		}

		for(; nextJ < match.j; ++nextJ) {
			ret.match.total += approxTotalWeight(b[nextJ]);
		}

		++nextI;
		++nextJ;
		++id;
	}

	for(; nextI < a.size(); ++nextI) {
		ret.match.total += approxTotalWeight(a[nextI]);
	}

	for(; nextJ < b.size(); ++nextJ) {
		ret.match.total += approxTotalWeight(b[nextJ]);
	}

	return ret;
}

// finding
using RelIDPair = std::pair<const std::string_view, u32>;
using RelIDMap = std::unordered_map<std::string_view, u32,
		std::hash<std::string_view>,
		std::equal_to<std::string_view>,
		LinearScopedAllocator<RelIDPair>>;

u32 evaluateRelID(RelIDMap& ids, const Command& start, const Command& dst) {
	auto* cmd = &start;
	while(cmd) {
		auto it = ids.find(cmd->nameDesc());
		if(it == ids.end()) {
			it = ids.emplace(cmd->nameDesc(), 0u).first;
		} else {
			++it->second;
		}

		if(cmd == &dst) {
			return it->second;
		}

		cmd = cmd->next;
	}

	dlg_error("unreachable");
	return 0xFFFFFFFFu;
}

FindResult find(const Command& srcParent, const Command& src,
		const Command& dstParent, span<const Command*> dst,
		const CommandDescriptorSnapshot& dstDsState, float threshold) {
	ZoneScoped;

	dlg_assert_or(!dst.empty(), return {});
	dlg_assert(srcParent.isChild(src));

	std::vector<const Command*> bestCmds;
	float bestMatch = threshold;

	struct EvaluatedRelativeIDs {
		u32 dst;
		u32 best;
	};

	std::optional<EvaluatedRelativeIDs> relIDs;

	ThreadMemScope tms;
	RelIDMap relIDMap{tms};

	for(auto it = &src; it; it = it->next) {
		auto m = it->match(*dst[0]);
		auto em = eval(m);

		// dlg_trace("em: {} {} - {}", em, it->toString(), dst[0]->toString());

		// NOTE: early-continue on less-than-best here incorrect for
		// non-parent commands as we might increase the matching value below.
		if(em == 0.f || (em < bestMatch && dst.size() > 1)) {
			continue;
		}

#ifdef VIL_COMMAND_CALLSTACKS
		// NOTE WIP: use callstacks for matching.
		// Really hard-reject if they aren't the same?
		// Should probably make this an option, there might be
		// special cases I'm not thinkin of rn.
		if(!same(it->stacktrace, dst[0]->stacktrace)) {
			continue;
		}
#endif // VIL_COMMAND_CALLSTACKS

		std::vector<const Command*> currCmds {it};
		float childMatch = 1.f;

		if(dst.size() > 1) {
			dlg_assert(it->children());
			auto newThresh = bestMatch / em;
			auto restResult = find(*it, *it->children(),
				*dst[0], dst.subspan(1), dstDsState, newThresh);
			if(restResult.hierarchy.empty()) {
				// no candidate found
				continue;
			}

			auto& rest = restResult.hierarchy;
			currCmds.insert(currCmds.end(), rest.begin(), rest.end());
			childMatch = restResult.match;

			// TODO: replace dynamic_cast with some 'isStateCmd(const Command&)'
			//  check that simply checks for category (draw | dispatch | traceRays)
		} else if(auto srcCmd = dynamic_cast<const StateCmdBase*>(it); srcCmd) {
			// match descriptors, if any
			// TODO: only consider descriptors statically used by pipeline
			// NOTE: the `min(dsCount, ...)` is used defensively here to
			//   account for extensions that mess with bound-descriptor
			//   requirements, e.g. push descriptors.
			auto* dstCmd = dynamic_cast<const StateCmdBase*>(dst[0]);
			dlg_assert_or(dstCmd, continue);

			auto* srcPipe = srcCmd->boundPipe();
			auto* dstPipe = dstCmd->boundPipe();
			dlg_assert_or(same(srcPipe, dstPipe), continue);

			auto srcDescriptors = srcCmd->boundDescriptors().descriptorSets;
			auto dstDescriptors = dstCmd->boundDescriptors().descriptorSets;

			auto dsCount = dstPipe->layout->descriptors.size();
			auto srcBound = srcDescriptors.first(std::min(dsCount, srcDescriptors.size()));
			auto dstBound = dstDescriptors.first(std::min(dsCount, dstDescriptors.size()));

			if(!dstBound.empty() || !srcBound.empty()) {
				// TODO: consider dynamic offsets?
				for(auto i = 0u; i < std::min(srcBound.size(), dstBound.size()); ++i) {
					if(!srcBound[i].dsEntry || !dstBound[i].dsEntry) {
						// TODO: not sure if this can happen. Do sets
						// that are statically not used by pipeline
						// have to be bound?
						dlg_warn("ds not bound? shouldn't happen");
						continue;
					}

					// We can safely directly access the BoundDescriptorSet
					// here since we can assume the given record to be valid.
					auto& srcDs = access(srcBound[i]);
					auto dstDsCow = dstDsState.states.find(dstBound[i].dsEntry);
					// TODO: we might not find it here due to the new
					// descriptor set capturing rework.
					if(dstDsCow == dstDsState.states.end()) {
						continue;
					}

					auto [dstDs, lock] = access(*dstDsCow->second);

					auto res = vil::match(srcDs, dstDs);
					m.match += res.match;
					m.total += res.total;
				}

				// auto dsMatch = float(match) / std::max(srcBound.size(), dstBound.size());
				// dlg_trace("match: {}, dsMatch: {}", m, dsMatch);
			}
		}

		em = eval(m) * childMatch;

		if(em == 0.f || em < bestMatch) {
			continue;
		} else if(em == bestMatch && !bestCmds.empty()) {
			// When the match values of two commands are equal (which isn't
			// too unlikely, the default Command::match implementation
			// simply returns 1.f when the types are equal), choose
			// simply by order in current hierarchy level.
			if(!relIDs) {
				auto& rids = relIDs.emplace();
				RelIDMap tmpMap{tms};
				rids.dst = evaluateRelID(tmpMap,
					*dstParent.children(), *dst[0]);

				rids.best = evaluateRelID(relIDMap,
					*srcParent.children(), *bestCmds[0]);
			}

			// use bestCmds[0]->next instead of srcParent.children() and reuse relIDMap
			// to not iterate over *all* commands again.
			auto currRelID = evaluateRelID(relIDMap,
				*bestCmds[0]->next, *currCmds[0]);
			if(std::abs(int(currRelID) - int(relIDs->dst)) >=
					std::abs(int(relIDs->best) - int(relIDs->dst))) {
				continue;
			}

			relIDs->best = currRelID;
		}

		bestCmds.clear();
		bestCmds = std::move(currCmds);
		bestMatch = em;
	}

	return {bestCmds, bestMatch};
}

FindResult find(const ParentCommand& srcRoot, span<const Command*> dstHierarchyToFind,
		const CommandDescriptorSnapshot& dstDescriptors, float threshold) {
	// empty hierarchy
	if(!srcRoot.children()) {
		return {};
	}

	// otherwise only the root command would be selected, makes no sense
	dlg_assert(dstHierarchyToFind.size() >= 2);
	dlg_assert(dynamic_cast<const ParentCommand*>(dstHierarchyToFind[0]));

	auto ret = find(srcRoot, *srcRoot.children(),
		*dstHierarchyToFind[0], dstHierarchyToFind.subspan(1),
		dstDescriptors, threshold);
	if(!ret.hierarchy.empty()) {
		ret.hierarchy.insert(ret.hierarchy.begin(), &srcRoot);
	}

	return ret;
}

} // namespace vil
