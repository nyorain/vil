#pragma once

#include <fwd.hpp>
#include <vector>

namespace imgio { class Stream; }

namespace vil {

// We want to serialize CompletedHook objects
// - record
//   - commands
//     - handles referenced by commands?
//       we only really need this for the selected command though
// - for the selected command we need all referenced handles
// 	 - pipelines, when allowing shader debugging: shader mods -> spirv code
// 	 - accelStruct: dump geometry (BLAS) or references to other accelStructs (TLAS)
// 	 - image, buffer: no need to retrieve content or something, that is done
// 	   via CommandHookState. Only need properties
// - CommandHookState
//   - OwnBuffer: easy, just dump the copied content
//   - CopiedImage: more difficult, use in-file ktx?
//   - CopiedImageToBuffer:
// - CommandDescriptorSnapshot
//   - descriptor state
//     - the handles referenced by the descriptors

// Resulting file structure:
// - header
//   - version, device, instance information
// - handles:
// 	 - for type in {image, buffer, pipeline, shader, ...}
// 	   - u64 handleType
// 	   - u64 numHandles
// 	   - handle[numHandles]
// - the selected record
// 	  - record header, meta information
// 	  - u64 sizeInBytes
// 	  - for each command in hierarchy
// 	  	- u64 commandType
// 	  	- data depending on command
// 	  	  they reference handles via handleType-specific ids
// - CommandHookState
//   - TODO: all fields or just have a list of optional fields?
//   - buffer state: (u64 sizeInBytes, byte data[sizeInBytes])
//   - image state: full ktx2 file including header and everything
//     just write out via imgio
//   - bufferToImageState: (u64 format, Buffer)
// - DescriptorState: TODO: not sure about this one. Even needed?
//   Just dump out the referenced descriptorSets and embed the state?
//   But harder to read when loading the capture, we need the descriptorSnapshot
//   then...

class CompletedHookSerializer {
public:
	std::vector<std::byte> serialize(CompletedHook&);

private:
	std::vector<std::byte> commandData;

	// handles
	std::vector<Image*> images;
	std::vector<Buffer*> buffers;
	std::vector<ImageView*> imageViews;
	std::vector<BufferView*> bufferViews;
	std::vector<DescriptorLayout*> dsLayouts;
	std::vector<PipelineLayout*> pipeLayouts;
	std::vector<GraphicsPipeline*> gfxPipes;
	std::vector<ComputePipeline*> computePipes;
	std::vector<RayTracingPipeline*> rtPipes;
	std::vector<Event*> events;
	std::vector<RenderPass*> rps;
	std::vector<Framebuffer*> fbs;
};

} // namespace


#include <unordered_map>
#include <string_view>
#include <command/commands.hpp>

namespace vil {

template<typename Base>
struct Factory {
	using Creator = Base*(*)();
	std::unordered_map<std::string_view, Creator> types;
};

template<typename Base, typename Cmd>
Base* creatorImpl() {
	return new Cmd();
}

std::vector<std::byte> CompletedHookSerializer::serialize(CompletedHook&) {
	// TODO: init once
	Factory<Command> cmdFactory {{
		{"<RootCommand>", &creatorImpl<Command, RootCommand>},
			// ...
		{"WaitEvents", &creatorImpl<Command, WaitEventsCmd>},
	}};

}

} // namespace

// v2
// rethinking source files
// - create folder serialize/
// - serialize/util.hpp: base stuff, read/write, SaveBuf/LoadBuf
// - serialize/handles.hpp: serializes handles.
//   (header-only or add handles.cpp and explicit template instantiations)
// - serialize/serialize.hpp: main api, entrypoints
// - serialize/commands.hpp

// Stuff to pull fromm Command into visitors (later):
// - displayInspector?
// - match
// - record?
