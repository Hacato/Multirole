#include "LobbyListing.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <boost/asio/write.hpp>
#include <boost/json.hpp>
#include <fmt/format.h> // fmt::to_string

#include "../Lobby.hpp"
#include "../Workaround.hpp"

namespace Ignis::Multirole::Endpoint
{

namespace
{

constexpr std::string_view WINDOWS_UPDATE_PATH = "/client-update";
constexpr std::string_view ANDROID_UPDATE_PATH = "/android-client-update";

constexpr std::string_view REALM_USER_AGENT_MARKER = "-RealmOfKings-";

constexpr const char* REALM_WINDOWS_COMMIT_PATH =
	"./sync/realm_client/commit";

constexpr const char* REALM_WINDOWS_UPDATE_JSON_PATH =
	"./sync/realm_client/update.json";

constexpr const char* REALM_ANDROID_COMMIT_PATH =
	"./sync/realm_android/commit";

constexpr const char* REALM_ANDROID_UPDATE_JSON_PATH =
	"./sync/realm_android/update.json";

std::string TrimWhitespace(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if(first == std::string::npos)
		return {};

	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1U);
}

std::string ReadTextFile(const char* path)
{
	std::ifstream file(path, std::ios::binary);
	if(!file)
		return {};

	return std::string(
		std::istreambuf_iterator<char>(file),
		std::istreambuf_iterator<char>());
}

std::string MakeJsonResponse(
	std::string_view body,
	std::string_view status = "200 OK")
{
	return fmt::format(
		"HTTP/1.0 {}\r\n"
		"Content-Length: {}\r\n"
		"Content-Type: application/json\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n"
		"{}",
		status,
		body.size(),
		body);
}

std::string_view GetRequestPath(std::string_view request)
{
	const auto lineEnd = request.find("\r\n");
	const auto firstLine = request.substr(0, lineEnd);

	const auto firstSpace = firstLine.find(' ');
	if(firstSpace == std::string_view::npos)
		return {};

	const auto secondSpace = firstLine.find(' ', firstSpace + 1U);
	if(secondSpace == std::string_view::npos)
		return {};

	return firstLine.substr(
		firstSpace + 1U,
		secondSpace - firstSpace - 1U);
}

std::string_view GetHeader(
	std::string_view request,
	std::string_view headerName)
{
	std::size_t pos = 0;

	while(pos < request.size())
	{
		const auto lineEnd = request.find("\r\n", pos);

		const auto end =
			lineEnd == std::string_view::npos
				? request.size()
				: lineEnd;

		const auto line = request.substr(pos, end - pos);

		if(line.size() > headerName.size() &&
		   line.substr(0, headerName.size()) == headerName &&
		   line[headerName.size()] == ':')
		{
			auto value = line.substr(headerName.size() + 1U);

			while(!value.empty() &&
			      (value.front() == ' ' ||
			       value.front() == '\t'))
			{
				value.remove_prefix(1U);
			}

			return value;
		}

		if(lineEnd == std::string_view::npos)
			break;

		pos = lineEnd + 2U;
	}

	return {};
}

std::string_view GetRealmBuildCommit(
	std::string_view userAgent)
{
	const auto marker =
		userAgent.find(REALM_USER_AGENT_MARKER);

	if(marker == std::string_view::npos)
		return {};

	auto commit =
		userAgent.substr(
			marker + REALM_USER_AGENT_MARKER.size());

	const auto end =
		commit.find_first_of(" \t\r\n");

	if(end != std::string_view::npos)
		commit = commit.substr(0, end);

	return commit;
}

std::string MakeUpdateResponse(
	std::string_view request,
	const char* commitPath,
	const char* updateJsonPath)
{
	const auto currentCommit =
		TrimWhitespace(ReadTextFile(commitPath));

	const auto userAgent =
		GetHeader(request, "User-Agent");

	const auto clientCommit =
		GetRealmBuildCommit(userAgent);

	if(currentCommit.empty())
	{
		// Fail closed if the server cannot determine
		// the current release for this platform.
		return MakeJsonResponse("[]");
	}

	if(!clientCommit.empty() &&
	   clientCommit == currentCommit)
	{
		return MakeJsonResponse("[]");
	}

	const auto updateJson =
		ReadTextFile(updateJsonPath);

	if(updateJson.empty())
		return MakeJsonResponse("[]");

	// Validate the generated update file before
	// sending it to EDOPro.
	boost::system::error_code ec;

	const auto parsed =
		boost::json::parse(updateJson, ec);

	if(ec || !parsed.is_array())
		return MakeJsonResponse("[]");

	return MakeJsonResponse(updateJson);
}

} // namespace

class LobbyListing::Connection final
	: public std::enable_shared_from_this<Connection>
{
public:
	Connection(
		boost::asio::ip::tcp::socket socket,
		std::shared_ptr<const std::string> roomListData) noexcept
		:
		socket(std::move(socket)),
		roomListData(std::move(roomListData)),
		incoming(),
		request(),
		writeCalled(false)
	{}

	void DoRead() noexcept
	{
		auto self(shared_from_this());

		socket.async_read_some(
			boost::asio::buffer(incoming),
			[this, self](
				boost::system::error_code ec,
				std::size_t bytesRead)
			{
				if(ec)
					return;

				request.append(
					incoming.data(),
					bytesRead);

				// Wait until the complete HTTP header
				// has been received.
				if(request.find("\r\n\r\n") ==
				   std::string::npos)
				{
					if(request.size() > 16384U)
					{
						writeCalled = true;

						DoWrite(
							std::make_shared<
								const std::string>(
								MakeJsonResponse(
									"[]",
									"400 Bad Request")));

						return;
					}

					DoRead();
					return;
				}

				if(writeCalled)
					return;

				writeCalled = true;

				const auto path =
					GetRequestPath(request);

				// Android must be checked first because
				// "/android-client-update" contains the
				// text "/client-update".
				if(path.find(ANDROID_UPDATE_PATH) !=
				   std::string_view::npos)
				{
					DoWrite(
						std::make_shared<
							const std::string>(
							MakeUpdateResponse(
								request,
								REALM_ANDROID_COMMIT_PATH,
								REALM_ANDROID_UPDATE_JSON_PATH)));

					return;
				}

				// Windows Realm updater.
				if(path.find(WINDOWS_UPDATE_PATH) !=
				   std::string_view::npos)
				{
					DoWrite(
						std::make_shared<
							const std::string>(
							MakeUpdateResponse(
								request,
								REALM_WINDOWS_COMMIT_PATH,
								REALM_WINDOWS_UPDATE_JSON_PATH)));

					return;
				}

				// Preserve the original room-list behavior
				// for every other path.
				DoWrite(roomListData);
			});
	}

private:
	boost::asio::ip::tcp::socket socket;
	std::shared_ptr<const std::string> roomListData;
	std::array<char, 1024U> incoming;
	std::string request;
	bool writeCalled;

	void DoWrite(
		std::shared_ptr<const std::string> outgoing) noexcept
	{
		auto self(shared_from_this());

		boost::asio::async_write(
			socket,
			boost::asio::buffer(*outgoing),
			[this, self, outgoing](
				boost::system::error_code ec,
				std::size_t /*unused*/)
			{
				if(!ec)
				{
					socket.shutdown(
						boost::asio::ip::tcp::socket::
							shutdown_both,
						ec);
				}
			});
	}
};

// public

LobbyListing::LobbyListing(
	boost::asio::io_context& ioCtx,
	unsigned short port,
	Lobby& lobby)
	:
	acceptor(
		ioCtx,
		boost::asio::ip::tcp::endpoint(
			boost::asio::ip::tcp::v6(),
			port)),
	serializeTimer(ioCtx),
	lobby(lobby),
	serialized(std::make_shared<std::string>())
{
	Workaround::SetCloseOnExec(
		acceptor.native_handle());

	acceptor.set_option(
		boost::asio::socket_base::keep_alive(true));

	DoAccept();
	DoSerialize();
}

LobbyListing::~LobbyListing() = default;

void LobbyListing::Stop()
{
	acceptor.close();
	serializeTimer.cancel();
}

// private

void LobbyListing::DoSerialize()
{
	serializeTimer.expires_after(
		std::chrono::seconds(2));

	serializeTimer.async_wait(
		[this](boost::system::error_code ec)
		{
			if(ec)
				return;

			boost::json::monotonic_resource mr;
			boost::json::object j(&mr);

			auto& ar =
				*j.emplace(
					"rooms",
					boost::json::array(&mr))
					.first->value().if_array();

			lobby.CollectRooms(
				[&](const Lobby::RoomProps& rp)
				{
					const auto dCount =
						rp.duelists.usedCount;

					if(dCount == 0)
						return;

					const auto& hi =
						*rp.hostInfo;

					auto& room =
						*ar.emplace_back(
							boost::json::object(
								21U,
								&mr))
							.if_object();

					room.emplace("roomid", rp.id);
					room.emplace("roomname", "");
					room.emplace("roomnotes", *rp.notes);
					room.emplace("roommode", 0);
					room.emplace(
						"needpass",
						rp.passworded);
					room.emplace(
						"team1",
						hi.t0Count);
					room.emplace(
						"team2",
						hi.t1Count);
					room.emplace(
						"best_of",
						hi.bestOf);
					room.emplace(
						"duel_flag",
						YGOPro::HostInfo::
							OrDuelFlags(
								hi.duelFlagsHigh,
								hi.duelFlagsLow));
					room.emplace(
						"forbidden_types",
						hi.forb);
					room.emplace(
						"extra_rules",
						hi.extraRules);
					room.emplace(
						"start_lp",
						hi.startingLP);
					room.emplace(
						"start_hand",
						hi.startingDrawCount);
					room.emplace(
						"draw_count",
						hi.drawCountPerTurn);
					room.emplace(
						"time_limit",
						hi.timeLimitInSeconds);
					room.emplace(
						"rule",
						hi.allowed);
					room.emplace(
						"no_check",
						static_cast<bool>(
							hi.dontCheckDeckContent));
					room.emplace(
						"no_shuffle",
						static_cast<bool>(
							hi.dontShuffleDeck));
					room.emplace(
						"banlist_hash",
						hi.banlistHash);
					room.emplace(
						"istart",
						rp.started
							? "start"
							: "waiting");
					room.emplace(
						"main_min",
						hi.limits.main.min);
					room.emplace(
						"main_max",
						hi.limits.main.max);
					room.emplace(
						"extra_min",
						hi.limits.extra.min);
					room.emplace(
						"extra_max",
						hi.limits.extra.max);
					room.emplace(
						"side_min",
						hi.limits.side.min);
					room.emplace(
						"side_max",
						hi.limits.side.max);

					auto& ac =
						*room.emplace(
							"users",
							boost::json::array(
								dCount,
								&mr))
							.first->value().if_array();

					for(std::size_t i = 0;
					    i < dCount;
					    i++)
					{
						auto& client =
							ac[i].emplace_object();

						client.emplace(
							"pos",
							rp.duelists.pairs[i].pos);

						client.emplace(
							"name",
							std::string_view{
								rp.duelists.pairs[i]
									.name.data(),
								rp.duelists.pairs[i]
									.nameLength});
					}
				});

			{
				constexpr const char* const
					HTTP_HEADER_FORMAT_STRING =
						"HTTP/1.0 200 OK\r\n"
						"Content-Length: {:d}\r\n"
						"Content-Type: application/json\r\n"
						"\r\n";

				const auto strJ =
					boost::json::serialize(j);

				auto full =
					fmt::format(
						HTTP_HEADER_FORMAT_STRING,
						strJ.size());

				full += strJ;

				std::scoped_lock lock(mSerialized);

				serialized =
					std::make_shared<
						const std::string>(
						std::move(full));
			}

			DoSerialize();
		});
}

void LobbyListing::DoAccept()
{
	acceptor.async_accept(
		[this](
			const boost::system::error_code& ec,
			boost::asio::ip::tcp::socket socket)
		{
			if(!acceptor.is_open())
				return;

			if(!ec)
			{
				Workaround::SetCloseOnExec(
					socket.native_handle());

				std::scoped_lock lock(mSerialized);

				std::make_shared<Connection>(
					std::move(socket),
					serialized)
					->DoRead();
			}

			DoAccept();
		});
}

} // namespace Ignis::Multirole::Endpoint
