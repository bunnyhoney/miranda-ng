//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/MessagesManager.h"

#include "td/telegram/secret_api.hpp"
#include "td/telegram/td_api.hpp"
#include "td/telegram/telegram_api.h"

#include "td/actor/PromiseFuture.h"
#include "td/actor/SleepActor.h"

#include "td/db/binlog/BinlogHelper.h"

#include "td/telegram/AnimationsManager.h"
#include "td/telegram/AnimationsManager.hpp"
#include "td/telegram/AudiosManager.h"
#include "td/telegram/AudiosManager.hpp"
#include "td/telegram/AuthManager.h"
#include "td/telegram/ConfigShared.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/DialogDb.h"
#include "td/telegram/DocumentsManager.h"
#include "td/telegram/DocumentsManager.hpp"
#include "td/telegram/files/FileManager.h"
#include "td/telegram/Game.h"
#include "td/telegram/Game.hpp"
#include "td/telegram/Global.h"
#include "td/telegram/HashtagHints.h"
#include "td/telegram/InlineQueriesManager.h"
#include "td/telegram/logevent/LogEvent.h"
#include "td/telegram/MessagesDb.h"
#include "td/telegram/misc.h"
#include "td/telegram/net/NetActor.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/Payments.h"
#include "td/telegram/Payments.hpp"
#include "td/telegram/ReplyMarkup.hpp"
#include "td/telegram/SecretChatActor.h"
#include "td/telegram/SecretChatsManager.h"
#include "td/telegram/SequenceDispatcher.h"
#include "td/telegram/StickersManager.h"
#include "td/telegram/StickersManager.hpp"
#include "td/telegram/Td.h"
#include "td/telegram/TopDialogManager.h"
#include "td/telegram/UpdatesManager.h"
#include "td/telegram/VideoNotesManager.h"
#include "td/telegram/VideoNotesManager.hpp"
#include "td/telegram/VideosManager.h"
#include "td/telegram/VideosManager.hpp"
#include "td/telegram/VoiceNotesManager.h"
#include "td/telegram/VoiceNotesManager.hpp"
#include "td/telegram/WebPageId.h"
#include "td/telegram/WebPagesManager.h"

#include "td/utils/format.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/MimeType.h"
#include "td/utils/misc.h"
#include "td/utils/PathView.h"
#include "td/utils/Random.h"
#include "td/utils/Slice.h"
#include "td/utils/Time.h"
#include "td/utils/tl_helpers.h"
#include "td/utils/tl_storers.h"
#include "td/utils/utf8.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace td {

class NetActorOnce : public NetActor {
  void hangup() override {
    on_error(0, Status::Error(500, "Request aborted"));
    stop();
  }

  void on_result_finish() override {
    stop();
  }
};

void dummyUpdate::store(TlStorerToString &s, const char *field_name) const {
  s.store_class_begin(field_name, "dummyUpdate");
  s.store_class_end();
}

class updateSentMessage : public telegram_api::Update {
 public:
  int64 random_id_;

  MessageId message_id_;
  int32 date_;

  updateSentMessage(int64 random_id, MessageId message_id, int32 date)
      : random_id_(random_id), message_id_(message_id), date_(date) {
  }

  static constexpr int32 ID = 1234567890;
  int32 get_id() const override {
    return ID;
  }

  void store(TlStorerUnsafe &s) const override {
    UNREACHABLE();
  }

  void store(TlStorerCalcLength &s) const override {
    UNREACHABLE();
  }

  void store(TlStorerToString &s, const char *field_name) const override {
    s.store_class_begin(field_name, "updateSentMessage");
    s.store_field("random_id_", random_id_);
    s.store_field("message_id_", message_id_.get());
    s.store_field("date_", date_);
    s.store_class_end();
  }
};

class GetDialogQuery : public Td::ResultHandler {
  DialogId dialog_id_;

 public:
  void send(DialogId dialog_id) {
    dialog_id_ = dialog_id;
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_getPeerDialogs(
        td->messages_manager_->get_input_dialog_peers({dialog_id}, AccessRights::Read)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getPeerDialogs>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive chat: " << to_string(result);

    td->contacts_manager_->on_get_chats(std::move(result->chats_));
    td->contacts_manager_->on_get_users(std::move(result->users_));
    td->messages_manager_->on_get_dialogs(
        std::move(result->dialogs_), -1, std::move(result->messages_),
        PromiseCreator::lambda([td = td, dialog_id = dialog_id_](Result<> result) {
          if (result.is_ok()) {
            td->messages_manager_->on_get_dialog_success(dialog_id);
          } else {
            if (G()->close_flag()) {
              return;
            }
            td->messages_manager_->on_get_dialog_error(dialog_id, result.error(), "OnGetDialogs");
            td->messages_manager_->on_get_dialog_fail(dialog_id, result.move_as_error());
          }
        }));
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetDialogQuery");
    td->messages_manager_->on_get_dialog_fail(dialog_id_, std::move(status));
  }
};

class GetPinnedDialogsQuery : public NetActorOnce {
  Promise<Unit> promise_;

 public:
  explicit GetPinnedDialogsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  NetQueryRef send(uint64 sequence_id) {
    auto query = G()->net_query_creator().create(create_storer(telegram_api::messages_getPinnedDialogs()));
    auto result = query.get_weak();
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_id);
    return result;
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getPinnedDialogs>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive pinned chats: " << to_string(result);

    td->contacts_manager_->on_get_chats(std::move(result->chats_));
    td->contacts_manager_->on_get_users(std::move(result->users_));
    std::reverse(result->dialogs_.begin(), result->dialogs_.end());
    td->messages_manager_->on_get_dialogs(std::move(result->dialogs_), -2, std::move(result->messages_),
                                          std::move(promise_));
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};
/*
class GetDialogsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit GetDialogsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<DialogId> &&dialog_ids) {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_getPeerDialogs(
        td->messages_manager_->get_input_dialog_peers(dialog_ids, AccessRights::Read)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getPeerDialogs>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive chats: " << to_string(result);

    td->contacts_manager_->on_get_chats(std::move(result->chats_));
    td->contacts_manager_->on_get_users(std::move(result->users_));
    td->messages_manager_->on_get_dialogs(std::move(result->dialogs_), -1, std::move(result->messages_),
                                          std::move(promise_));
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};
*/
class GetMessagesQuery : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit GetMessagesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<tl_object_ptr<telegram_api::InputMessage>> &&message_ids) {
    send_query(
        G()->net_query_creator().create(create_storer(telegram_api::messages_getMessages(std::move(message_ids)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getMessages>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    // LOG(INFO) << "Receive result for GetMessagesQuery " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages(std::move(messages->messages_), false, "get messages");
        break;
      }
      case telegram_api::messages_messagesSlice::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messagesSlice>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages(std::move(messages->messages_), false, "get messages slice");
        break;
      }
      case telegram_api::messages_channelMessages::ID: {
        LOG(ERROR) << "Receive channel messages in GetMessagesQuery";
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages(std::move(messages->messages_), false, "get channel messages");
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (status.message() == "MESSAGE_IDS_EMPTY") {
      promise_.set_value(Unit());
      return;
    }
    promise_.set_error(std::move(status));
  }
};

class GetChannelMessagesQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit GetChannelMessagesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, tl_object_ptr<telegram_api::InputChannel> &&input_channel,
            vector<tl_object_ptr<telegram_api::InputMessage>> &&message_ids) {
    channel_id_ = channel_id;
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::channels_getMessages(std::move(input_channel), std::move(message_ids)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_getMessages>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetChannelMessagesQuery " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID: {
        LOG(ERROR) << "Receive ordinary messages in GetChannelMessagesQuery";
        auto messages = move_tl_object_as<telegram_api::messages_messages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages(std::move(messages->messages_), true, "get messages");
        break;
      }
      case telegram_api::messages_messagesSlice::ID: {
        LOG(ERROR) << "Receive ordinary messages in GetChannelMessagesQuery";
        auto messages = move_tl_object_as<telegram_api::messages_messagesSlice>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages(std::move(messages->messages_), true, "get messages slice");
        break;
      }
      case telegram_api::messages_channelMessages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages(std::move(messages->messages_), true, "get channel messages");
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (status.message() == "MESSAGE_IDS_EMPTY") {
      promise_.set_value(Unit());
      return;
    }
    td->contacts_manager_->on_get_channel_error(channel_id_, status, "GetChannelMessagesQuery");
    promise_.set_error(std::move(status));
  }
};

class GetChannelPinnedMessageQuery : public Td::ResultHandler {
  Promise<MessageId> promise_;
  ChannelId channel_id_;

 public:
  explicit GetChannelPinnedMessageQuery(Promise<MessageId> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id) {
    auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
    if (input_channel == nullptr) {
      return promise_.set_error(Status::Error(6, "Can't access the chat"));
    }

    channel_id_ = channel_id;
    vector<tl_object_ptr<telegram_api::InputMessage>> input_messages;
    input_messages.push_back(make_tl_object<telegram_api::inputMessagePinned>());
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::channels_getMessages(std::move(input_channel), std::move(input_messages)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_getMessages>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetChannelPinnedMessageQuery " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID:
      case telegram_api::messages_messagesSlice::ID:
        LOG(ERROR) << "Receive ordinary messages in GetChannelPinnedMessageQuery " << to_string(ptr);
        return promise_.set_error(Status::Error(500, "Receive wrong request result"));
      case telegram_api::messages_channelMessages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        if (messages->messages_.empty()) {
          return promise_.set_value(MessageId());
        }
        if (messages->messages_.size() >= 2) {
          LOG(ERROR) << to_string(ptr);
          return promise_.set_error(Status::Error(500, "More than 1 pinned message received"));
        }
        auto full_message_id = td->messages_manager_->on_get_message(std::move(messages->messages_[0]), false, true,
                                                                     false, false, "get channel pinned messages");
        if (full_message_id.get_dialog_id().is_valid() && full_message_id.get_dialog_id() != DialogId(channel_id_)) {
          LOG(ERROR) << full_message_id << " " << to_string(ptr);
          return promise_.set_error(Status::Error(500, "Receive pinned message in a wrong chat"));
        }
        return promise_.set_value(full_message_id.get_message_id());
      }
      default:
        UNREACHABLE();
    }
  }

  void on_error(uint64 id, Status status) override {
    if (status.message() == "MESSAGE_IDS_EMPTY") {
      promise_.set_value(MessageId());
      return;
    }
    td->contacts_manager_->on_get_channel_error(channel_id_, status, "GetChannelPinnedMessageQuery");
    promise_.set_error(std::move(status));
  }
};

class ExportChannelMessageLinkQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  MessageId message_id_;
  bool for_group_;

 public:
  explicit ExportChannelMessageLinkQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, MessageId message_id, bool for_group) {
    channel_id_ = channel_id;
    message_id_ = message_id;
    for_group_ = for_group;
    auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(create_storer(telegram_api::channels_exportMessageLink(
        std::move(input_channel), message_id.get_server_message_id().get(), for_group))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_exportMessageLink>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ExportChannelMessageLinkQuery " << to_string(ptr);
    td->messages_manager_->on_get_public_message_link({DialogId(channel_id_), message_id_}, for_group_,
                                                      std::move(ptr->link_), std::move(ptr->html_));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->contacts_manager_->on_get_channel_error(channel_id_, status, "ExportChannelMessageLinkQuery");
    promise_.set_error(std::move(status));
  }
};

class GetDialogListQuery : public NetActorOnce {
  Promise<Unit> promise_;

 public:
  explicit GetDialogListQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int32 offset_date, ServerMessageId offset_message_id, DialogId offset_dialog_id, int32 limit,
            uint64 sequence_id) {
    auto input_peer = td->messages_manager_->get_input_peer(offset_dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      input_peer = make_tl_object<telegram_api::inputPeerEmpty>();
    }

    int32 flags = telegram_api::messages_getDialogs::EXCLUDE_PINNED_MASK;
    auto query = G()->net_query_creator().create(create_storer(telegram_api::messages_getDialogs(
        flags, false /*ignored*/, offset_date, offset_message_id.get(), std::move(input_peer), limit)));
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_id);
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getDialogs>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetDialogListQuery " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    auto promise = PromiseCreator::lambda(
        [promise = std::move(promise_)](Result<> result) mutable { promise.set_result(std::move(result)); });
    if (constructor_id == telegram_api::messages_dialogs::ID) {
      auto dialogs = move_tl_object_as<telegram_api::messages_dialogs>(ptr);
      td->contacts_manager_->on_get_chats(std::move(dialogs->chats_));
      td->contacts_manager_->on_get_users(std::move(dialogs->users_));
      td->messages_manager_->on_get_dialogs(std::move(dialogs->dialogs_), narrow_cast<int32>(dialogs->dialogs_.size()),
                                            std::move(dialogs->messages_), std::move(promise));
    } else {
      CHECK(constructor_id == telegram_api::messages_dialogsSlice::ID);
      auto dialogs = move_tl_object_as<telegram_api::messages_dialogsSlice>(ptr);
      td->contacts_manager_->on_get_chats(std::move(dialogs->chats_));
      td->contacts_manager_->on_get_users(std::move(dialogs->users_));
      td->messages_manager_->on_get_dialogs(std::move(dialogs->dialogs_), max(dialogs->count_, 0),
                                            std::move(dialogs->messages_), std::move(promise));
    }
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class SearchPublicDialogsQuery : public Td::ResultHandler {
  string query_;

 public:
  void send(const string &query) {
    query_ = query;
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::contacts_search(query, 3 /* ignored server-side */))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::contacts_search>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto dialogs = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for SearchPublicDialogsQuery " << to_string(dialogs);
    td->contacts_manager_->on_get_chats(std::move(dialogs->chats_));
    td->contacts_manager_->on_get_users(std::move(dialogs->users_));
    td->messages_manager_->on_get_public_dialogs_search_result(query_, std::move(dialogs->my_results_),
                                                               std::move(dialogs->results_));
  }

  void on_error(uint64 id, Status status) override {
    LOG(ERROR) << "Receive error for SearchPublicDialogsQuery: " << status;
    td->messages_manager_->on_failed_public_dialogs_search(query_, std::move(status));
  }
};

class GetCommonDialogsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  UserId user_id_;

 public:
  explicit GetCommonDialogsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(UserId user_id, int32 offset_chat_id, int32 limit) {
    user_id_ = user_id;
    LOG(INFO) << "Get common dialogs with " << user_id << " from " << offset_chat_id << " with limit " << limit;

    auto input_user = td->contacts_manager_->get_input_user(user_id);
    CHECK(input_user != nullptr);

    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_getCommonChats(std::move(input_user), offset_chat_id, limit))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getCommonChats>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto chats_ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetCommonDialogsQuery " << to_string(chats_ptr);
    int32 constructor_id = chats_ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_chats::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chats>(chats_ptr);
        td->messages_manager_->on_get_common_dialogs(user_id_, std::move(chats->chats_),
                                                     narrow_cast<int32>(chats->chats_.size()));
        break;
      }
      case telegram_api::messages_chatsSlice::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chatsSlice>(chats_ptr);
        td->messages_manager_->on_get_common_dialogs(user_id_, std::move(chats->chats_), chats->count_);
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class CreateChatQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  int64 random_id_;

 public:
  explicit CreateChatQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<tl_object_ptr<telegram_api::InputUser>> &&input_users, const string &title, int64 random_id) {
    random_id_ = random_id;
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_createChat(std::move(input_users), title))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_createChat>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for createChat " << to_string(ptr);
    td->messages_manager_->on_create_new_dialog_success(random_id_, std::move(ptr), DialogType::Chat,
                                                        std::move(promise_));
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_create_new_dialog_fail(random_id_, std::move(status), std::move(promise_));
  }
};

class CreateChannelQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  int64 random_id_;

 public:
  explicit CreateChannelQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &title, bool is_megagroup, const string &about, int64 random_id) {
    int32 flags = 0;
    if (is_megagroup) {
      flags |= telegram_api::channels_createChannel::MEGAGROUP_MASK;
    } else {
      flags |= telegram_api::channels_createChannel::BROADCAST_MASK;
    }

    random_id_ = random_id;
    send_query(G()->net_query_creator().create(create_storer(
        telegram_api::channels_createChannel(flags, false /*ignored*/, false /*ignored*/, title, about))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_createChannel>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for createChannel " << to_string(ptr);
    td->messages_manager_->on_create_new_dialog_success(random_id_, std::move(ptr), DialogType::Channel,
                                                        std::move(promise_));
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_create_new_dialog_fail(random_id_, std::move(status), std::move(promise_));
  }
};

class EditDialogPhotoQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  FileId file_id_;
  DialogId dialog_id_;

 public:
  explicit EditDialogPhotoQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(FileId file_id, DialogId dialog_id, tl_object_ptr<telegram_api::InputChatPhoto> &&input_chat_photo) {
    CHECK(input_chat_photo != nullptr);
    file_id_ = file_id;
    dialog_id_ = dialog_id;

    switch (dialog_id.get_type()) {
      case DialogType::Chat:
        send_query(G()->net_query_creator().create(create_storer(
            telegram_api::messages_editChatPhoto(dialog_id.get_chat_id().get(), std::move(input_chat_photo)))));
        break;
      case DialogType::Channel: {
        auto channel_id = dialog_id.get_channel_id();
        auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
        CHECK(input_channel != nullptr);
        send_query(G()->net_query_creator().create(
            create_storer(telegram_api::channels_editPhoto(std::move(input_channel), std::move(input_chat_photo)))));
        break;
      }
      default:
        UNREACHABLE();
    }
  }

  void on_result(uint64 id, BufferSlice packet) override {
    static_assert(std::is_same<telegram_api::messages_editChatPhoto::ReturnType,
                               telegram_api::channels_editPhoto::ReturnType>::value,
                  "");
    auto result_ptr = fetch_result<telegram_api::messages_editChatPhoto>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for editDialogPhoto: " << to_string(ptr);
    td->updates_manager_->on_get_updates(std::move(ptr));

    if (file_id_.is_valid()) {
      td->file_manager_->delete_partial_remote_location(file_id_);
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (file_id_.is_valid()) {
      td->file_manager_->delete_partial_remote_location(file_id_);
    }
    td->updates_manager_->get_difference("EditDialogPhotoQuery");

    if (status.message() == "CHAT_NOT_MODIFIED") {
      if (!td->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td->messages_manager_->on_get_dialog_error(dialog_id_, status, "EditDialogPhotoQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class EditDialogTitleQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit EditDialogTitleQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &title) {
    dialog_id_ = dialog_id;
    switch (dialog_id.get_type()) {
      case DialogType::Chat:
        send_query(G()->net_query_creator().create(
            create_storer(telegram_api::messages_editChatTitle(dialog_id.get_chat_id().get(), title))));
        break;
      case DialogType::Channel: {
        auto channel_id = dialog_id.get_channel_id();
        auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
        CHECK(input_channel != nullptr);
        send_query(G()->net_query_creator().create(
            create_storer(telegram_api::channels_editTitle(std::move(input_channel), title))));
        break;
      }
      default:
        UNREACHABLE();
    }
  }

  void on_result(uint64 id, BufferSlice packet) override {
    static_assert(std::is_same<telegram_api::messages_editChatTitle::ReturnType,
                               telegram_api::channels_editTitle::ReturnType>::value,
                  "");
    auto result_ptr = fetch_result<telegram_api::messages_editChatTitle>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for editDialogTitle " << to_string(ptr);
    td->updates_manager_->on_get_updates(std::move(ptr));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->updates_manager_->get_difference("EditDialogTitleQuery");

    if (status.message() == "CHAT_NOT_MODIFIED") {
      if (!td->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td->messages_manager_->on_get_dialog_error(dialog_id_, status, "EditDialogTitleQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class SaveDraftMessageQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit SaveDraftMessageQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const unique_ptr<DraftMessage> &draft_message) {
    LOG(INFO) << "Save draft in " << dialog_id;
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      LOG(INFO) << "Can't update draft message because have no write access to " << dialog_id;
      return;
    }

    int32 flags = 0;
    ServerMessageId reply_to_message_id;
    if (draft_message != nullptr) {
      if (draft_message->reply_to_message_id.is_valid() && draft_message->reply_to_message_id.is_server()) {
        reply_to_message_id = draft_message->reply_to_message_id.get_server_message_id();
        flags |= MessagesManager::SEND_MESSAGE_FLAG_IS_REPLY;
      }
      if (draft_message->input_message_text.disable_web_page_preview) {
        flags |= MessagesManager::SEND_MESSAGE_FLAG_DISABLE_WEB_PAGE_PREVIEW;
      }
      if (draft_message->input_message_text.text.entities.size()) {
        flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_ENTITIES;
      }
    }

    dialog_id_ = dialog_id;
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_saveDraft(
        flags, false /*ignored*/, reply_to_message_id.get(), std::move(input_peer),
        draft_message == nullptr ? "" : draft_message->input_message_text.text.text,
        draft_message == nullptr ? vector<tl_object_ptr<telegram_api::MessageEntity>>()
                                 : get_input_message_entities(td->contacts_manager_.get(),
                                                              draft_message->input_message_text.text.entities)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_saveDraft>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      on_error(id, Status::Error(400, "Save draft failed"));
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SaveDraftMessageQuery")) {
      LOG(ERROR) << "Receive error for SaveDraftMessageQuery: " << status;
    }
    promise_.set_error(std::move(status));
  }
};

class ToggleDialogPinQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  bool is_pinned_;

 public:
  explicit ToggleDialogPinQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, bool is_pinned) {
    dialog_id_ = dialog_id;
    is_pinned_ = is_pinned;
    auto input_peer = td->messages_manager_->get_input_dialog_peer(dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      return;
    }

    int32 flags = 0;
    if (is_pinned) {
      flags |= telegram_api::messages_toggleDialogPin::PINNED_MASK;
    }
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_toggleDialogPin(flags, false /*ignored*/, std::move(input_peer)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_toggleDialogPin>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      on_error(id, Status::Error(400, "Toggle dialog pin failed"));
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->messages_manager_->on_get_dialog_error(dialog_id_, status, "ToggleDialogPinQuery")) {
      LOG(ERROR) << "Receive error for ToggleDialogPinQuery: " << status;
    }
    td->messages_manager_->on_update_dialog_pinned(dialog_id_, !is_pinned_);
    promise_.set_error(std::move(status));
  }
};

class ReorderPinnedDialogsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit ReorderPinnedDialogsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const vector<DialogId> &dialog_ids) {
    int32 flags = telegram_api::messages_reorderPinnedDialogs::FORCE_MASK;
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_reorderPinnedDialogs(
        flags, true /*ignored*/, td->messages_manager_->get_input_dialog_peers(dialog_ids, AccessRights::Read)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_reorderPinnedDialogs>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.move_as_ok();
    if (!result) {
      return on_error(id, Status::Error(400, "Result is false"));
    }
    LOG(INFO) << "Pinned chats reordered";

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(ERROR) << "Receive error for ReorderPinnedDialogsQuery: " << status;
    td->messages_manager_->on_update_pinned_dialogs();
    promise_.set_error(std::move(status));
  }
};

class GetMessagesViewsQuery : public Td::ResultHandler {
  DialogId dialog_id_;
  vector<MessageId> message_ids_;

 public:
  void send(DialogId dialog_id, vector<MessageId> &&message_ids, bool increment_view_counter) {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      LOG(ERROR) << "Can't update message views because doesn't have info about the " << dialog_id;
      return;
    }

    LOG(INFO) << "View " << message_ids.size() << " messages in " << dialog_id
              << ", increment = " << increment_view_counter;
    dialog_id_ = dialog_id;
    message_ids_ = std::move(message_ids);
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_getMessagesViews(
        std::move(input_peer), MessagesManager::get_server_message_ids(message_ids_), increment_view_counter))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getMessagesViews>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    vector<int32> views = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetMessagesViewsQuery " << format::as_array(views);
    if (message_ids_.size() != views.size()) {
      return on_error(id, Status::Error(500, "Wrong number of message views returned"));
    }

    for (size_t i = 0; i < message_ids_.size(); i++) {
      td->messages_manager_->on_update_message_views({dialog_id_, message_ids_[i]}, views[i]);
    }
  }

  void on_error(uint64 id, Status status) override {
    if (!td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetMessagesViewsQuery")) {
      LOG(ERROR) << "Receive error for GetMessagesViewsQuery: " << status;
    }
    status.ignore();
  }
};

class ReadMessagesContentsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit ReadMessagesContentsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<MessageId> &&message_ids) {
    LOG(INFO) << "Receive ReadMessagesContentsQuery for messages " << format::as_array(message_ids);

    send_query(G()->net_query_creator().create(create_storer(
        telegram_api::messages_readMessageContents(MessagesManager::get_server_message_ids(message_ids)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_readMessageContents>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto affected_messages = result_ptr.move_as_ok();
    CHECK(affected_messages->get_id() == telegram_api::messages_affectedMessages::ID);

    if (affected_messages->pts_count_ > 0) {
      td->messages_manager_->add_pending_update(make_tl_object<dummyUpdate>(), affected_messages->pts_,
                                                affected_messages->pts_count_, false, "read messages content query");
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(ERROR) << "Receive error for read message contents: " << status;
    promise_.set_error(std::move(status));
  }
};

class ReadChannelMessagesContentsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit ReadChannelMessagesContentsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, vector<MessageId> &&message_ids) {
    channel_id_ = channel_id;

    auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
    if (input_channel == nullptr) {
      LOG(ERROR) << "Have no input channel for " << channel_id;
      return;
    }

    LOG(INFO) << "Receive ReadChannelMessagesContentsQuery for messages " << format::as_array(message_ids) << " in "
              << channel_id;

    send_query(G()->net_query_creator().create(create_storer(telegram_api::channels_readMessageContents(
        std::move(input_channel), MessagesManager::get_server_message_ids(message_ids)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_readMessageContents>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG_IF(ERROR, !result) << "Read channel messages contents failed";

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->contacts_manager_->on_get_channel_error(channel_id_, status, "ReadChannelMessagesContentsQuery")) {
      LOG(ERROR) << "Receive error for read messages contents in " << channel_id_ << ": " << status;
    }
    promise_.set_error(std::move(status));
  }
};

class GetDialogMessageByDateQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  int32 date_;
  int64 random_id_;

 public:
  explicit GetDialogMessageByDateQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, int32 date, int64 random_id) {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      return promise_.set_error(Status::Error(500, "Have no info about the chat"));
    }

    dialog_id_ = dialog_id;
    date_ = date;
    random_id_ = random_id;

    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_getHistory(std::move(input_peer), 0, date, -3, 5, 0, 0, 0))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getHistory>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messages>(ptr);
        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_dialog_message_by_date_success(dialog_id_, date_, random_id_,
                                                                     std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_messagesSlice::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messagesSlice>(ptr);
        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_dialog_message_by_date_success(dialog_id_, date_, random_id_,
                                                                     std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_channelMessages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);
        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_dialog_message_by_date_success(dialog_id_, date_, random_id_,
                                                                     std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_messagesNotModified::ID:
        return on_error(id, Status::Error(500, "Server returned messagesNotModified"));
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetDialogMessageByDateQuery")) {
      LOG(ERROR) << "Receive error for GetDialogMessageByDateQuery in " << dialog_id_ << ": " << status;
    }
    promise_.set_error(std::move(status));
    td->messages_manager_->on_get_dialog_message_by_date_fail(random_id_);
  }
};

class GetHistoryQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  MessageId from_message_id_;
  int32 offset_;
  int32 limit_;
  bool from_the_end_;

 public:
  explicit GetHistoryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId from_message_id, int32 offset, int32 limit) {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      LOG(ERROR) << "Can't get chat history in " << dialog_id << " because doesn't have info about the chat";
      return promise_.set_error(Status::Error(500, "Have no info about the chat"));
    }
    CHECK(offset < 0);

    dialog_id_ = dialog_id;
    from_message_id_ = from_message_id;
    offset_ = offset;
    limit_ = limit;
    from_the_end_ = false;
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_getHistory(
        std::move(input_peer), from_message_id.get_server_message_id().get(), 0, offset, limit, 0, 0, 0))));
  }

  void send_get_from_the_end(DialogId dialog_id, int32 limit) {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      LOG(ERROR) << "Can't get chat history because doesn't have info about the chat";
      return promise_.set_error(Status::Error(500, "Have no info about the chat"));
    }

    dialog_id_ = dialog_id;
    offset_ = 0;
    limit_ = limit;
    from_the_end_ = true;
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_getHistory(std::move(input_peer), 0, 0, 0, limit, 0, 0, 0))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getHistory>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    // LOG(INFO) << "Receive result for GetHistoryQuery " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_history(dialog_id_, from_message_id_, offset_, limit_, from_the_end_,
                                              std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_messagesSlice::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messagesSlice>(ptr);
        // TODO use messages->count_

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_history(dialog_id_, from_message_id_, offset_, limit_, from_the_end_,
                                              std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_channelMessages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);
        // TODO use messages->count_, messages->pts_

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_history(dialog_id_, from_message_id_, offset_, limit_, from_the_end_,
                                              std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_messagesNotModified::ID:
        LOG(ERROR) << "Receive messagesNotModified";
        break;
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetHistoryQuery")) {
      LOG(ERROR) << "Receive error for getHistoryQuery in " << dialog_id_ << ": " << status;
    }
    promise_.set_error(std::move(status));
  }
};

class ReadHistoryQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit ReadHistoryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId max_message_id) {
    dialog_id_ = dialog_id;
    send_query(G()->net_query_creator().create(create_storer(
        telegram_api::messages_readHistory(td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read),
                                           max_message_id.get_server_message_id().get()))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_readHistory>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto affected_messages = result_ptr.move_as_ok();
    CHECK(affected_messages->get_id() == telegram_api::messages_affectedMessages::ID);
    LOG(INFO) << "Receive result for readHistory: " << to_string(affected_messages);

    if (affected_messages->pts_count_ > 0) {
      td->messages_manager_->add_pending_update(make_tl_object<dummyUpdate>(), affected_messages->pts_,
                                                affected_messages->pts_count_, false, "read history query");
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->messages_manager_->on_get_dialog_error(dialog_id_, status, "ReadHistoryQuery")) {
      LOG(ERROR) << "Receive error for readHistory: " << status;
    }
    promise_.set_error(std::move(status));
  }
};

class ReadChannelHistoryQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  bool allow_error_;

 public:
  explicit ReadChannelHistoryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, MessageId max_message_id, bool allow_error) {
    channel_id_ = channel_id;
    allow_error_ = allow_error;
    auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    send_query(G()->net_query_creator().create(create_storer(
        telegram_api::channels_readHistory(std::move(input_channel), max_message_id.get_server_message_id().get()))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_readHistory>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG_IF(ERROR, !result && !allow_error_) << "Read history failed";

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->contacts_manager_->on_get_channel_error(channel_id_, status, "ReadChannelHistoryQuery")) {
      LOG(ERROR) << "Receive error for readChannelHistory: " << status;
    }
    promise_.set_error(std::move(status));
  }
};

class SearchMessagesQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  string query_;
  UserId sender_user_id_;
  MessageId from_message_id_;
  int32 offset_;
  int32 limit_;
  SearchMessagesFilter filter_;
  int64 random_id_;

 public:
  explicit SearchMessagesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &query, UserId sender_user_id,
            telegram_api::object_ptr<telegram_api::InputUser> &&sender_input_user, MessageId from_message_id,
            int32 offset, int32 limit, SearchMessagesFilter filter, int64 random_id) {
    auto input_peer = dialog_id.is_valid() ? td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read)
                                           : make_tl_object<telegram_api::inputPeerEmpty>();
    if (input_peer == nullptr) {
      LOG(ERROR) << "Can't search messages because doesn't have info about the chat";
      return promise_.set_error(Status::Error(500, "Have no info about the chat"));
    }

    dialog_id_ = dialog_id;
    query_ = query;
    sender_user_id_ = sender_user_id;
    from_message_id_ = from_message_id;
    offset_ = offset;
    limit_ = limit;
    filter_ = filter;
    random_id_ = random_id;

    if (filter == SearchMessagesFilter::UnreadMention) {
      send_query(G()->net_query_creator().create(create_storer(
          telegram_api::messages_getUnreadMentions(std::move(input_peer), from_message_id.get_server_message_id().get(),
                                                   offset, limit, std::numeric_limits<int32>::max(), 0))));
    } else {
      int32 flags = 0;
      if (sender_input_user != nullptr) {
        flags |= telegram_api::messages_search::FROM_ID_MASK;
      }

      send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_search(
          flags, std::move(input_peer), query, std::move(sender_input_user),
          MessagesManager::get_input_messages_filter(filter), 0, std::numeric_limits<int32>::max(),
          from_message_id.get_server_message_id().get(), offset, limit, std::numeric_limits<int32>::max(), 0, 0))));
    }
  }

  void on_result(uint64 id, BufferSlice packet) override {
    static_assert(std::is_same<telegram_api::messages_getUnreadMentions::ReturnType,
                               telegram_api::messages_search::ReturnType>::value,
                  "");
    auto result_ptr = fetch_result<telegram_api::messages_search>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for SearchMessagesQuery: " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_dialog_messages_search_result(
            dialog_id_, query_, sender_user_id_, from_message_id_, offset_, limit_, filter_, random_id_,
            narrow_cast<int32>(messages->messages_.size()), std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_messagesSlice::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messagesSlice>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_dialog_messages_search_result(
            dialog_id_, query_, sender_user_id_, from_message_id_, offset_, limit_, filter_, random_id_,
            messages->count_, std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_channelMessages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_dialog_messages_search_result(
            dialog_id_, query_, sender_user_id_, from_message_id_, offset_, limit_, filter_, random_id_,
            messages->count_, std::move(messages->messages_));
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SearchMessagesQuery");
    td->messages_manager_->on_failed_dialog_messages_search(dialog_id_, random_id_);
    promise_.set_error(std::move(status));
  }
};

class SearchMessagesGlobalQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  string query_;
  int32 offset_date_;
  DialogId offset_dialog_id_;
  MessageId offset_message_id_;
  int32 limit_;
  int64 random_id_;

 public:
  explicit SearchMessagesGlobalQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &query, int32 offset_date, DialogId offset_dialog_id, MessageId offset_message_id, int32 limit,
            int64 random_id) {
    query_ = query;
    offset_date_ = offset_date;
    offset_dialog_id_ = offset_dialog_id;
    offset_message_id_ = offset_message_id;
    limit_ = limit;
    random_id_ = random_id;

    auto input_peer = td->messages_manager_->get_input_peer(offset_dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      input_peer = make_tl_object<telegram_api::inputPeerEmpty>();
    }

    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_searchGlobal(
        query, offset_date_, std::move(input_peer), offset_message_id.get_server_message_id().get(), limit))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_searchGlobal>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for SearchMessagesGlobalQuery: " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages_search_result(
            query_, offset_date_, offset_dialog_id_, offset_message_id_, limit_, random_id_,
            narrow_cast<int32>(messages->messages_.size()), std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_messagesSlice::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messagesSlice>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages_search_result(query_, offset_date_, offset_dialog_id_,
                                                             offset_message_id_, limit_, random_id_, messages->count_,
                                                             std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_channelMessages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_messages_search_result(query_, offset_date_, offset_dialog_id_,
                                                             offset_message_id_, limit_, random_id_, messages->count_,
                                                             std::move(messages->messages_));
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_failed_messages_search(random_id_);
    promise_.set_error(std::move(status));
  }
};

class GetRecentLocationsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  int32 limit_;
  int64 random_id_;

 public:
  explicit GetRecentLocationsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, int32 limit, int64 random_id) {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      LOG(ERROR) << "Can't get recent locations because doesn't have info about the chat";
      return promise_.set_error(Status::Error(500, "Have no info about the chat"));
    }

    dialog_id_ = dialog_id;
    limit_ = limit;
    random_id_ = random_id;

    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_getRecentLocations(std::move(input_peer), limit, 0))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getRecentLocations>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetRecentLocationsQuery: " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_messages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_recent_locations(dialog_id_, limit_, random_id_,
                                                       narrow_cast<int32>(messages->messages_.size()),
                                                       std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_messagesSlice::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_messagesSlice>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_recent_locations(dialog_id_, limit_, random_id_, messages->count_,
                                                       std::move(messages->messages_));
        break;
      }
      case telegram_api::messages_channelMessages::ID: {
        auto messages = move_tl_object_as<telegram_api::messages_channelMessages>(ptr);

        td->contacts_manager_->on_get_chats(std::move(messages->chats_));
        td->contacts_manager_->on_get_users(std::move(messages->users_));
        td->messages_manager_->on_get_recent_locations(dialog_id_, limit_, random_id_, messages->count_,
                                                       std::move(messages->messages_));
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetRecentLocationsQuery");
    td->messages_manager_->on_get_recent_locations_failed(random_id_);
    promise_.set_error(std::move(status));
  }
};

class DeleteHistoryQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  MessageId max_message_id_;
  bool remove_from_dialog_list_;

  void send_request() {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id_, AccessRights::Read);
    if (input_peer == nullptr) {
      return promise_.set_error(Status::Error(3, "Chat is not accessible"));
    }

    int32 flags = 0;
    if (!remove_from_dialog_list_) {
      flags |= telegram_api::messages_deleteHistory::JUST_CLEAR_MASK;
    }
    LOG(INFO) << "Delete " << dialog_id_ << " history up to " << max_message_id_ << " with flags " << flags;

    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_deleteHistory(
        flags, false /*ignored*/, std::move(input_peer), max_message_id_.get_server_message_id().get()))));
  }

 public:
  explicit DeleteHistoryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId max_message_id, bool remove_from_dialog_list) {
    dialog_id_ = dialog_id;
    max_message_id_ = max_message_id;
    remove_from_dialog_list_ = remove_from_dialog_list;

    send_request();
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_deleteHistory>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto affected_history = result_ptr.move_as_ok();
    CHECK(affected_history->get_id() == telegram_api::messages_affectedHistory::ID);

    if (affected_history->pts_count_ > 0) {
      td->messages_manager_->add_pending_update(make_tl_object<dummyUpdate>(), affected_history->pts_,
                                                affected_history->pts_count_, false, "delete history query");
    }

    if (affected_history->offset_ > 0) {
      send_request();
      return;
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "DeleteHistoryQuery");
    promise_.set_error(std::move(status));
  }
};

class DeleteChannelHistoryQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  MessageId max_message_id_;
  bool allow_error_;

 public:
  explicit DeleteChannelHistoryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, MessageId max_message_id, bool allow_error) {
    channel_id_ = channel_id;
    max_message_id_ = max_message_id;
    allow_error_ = allow_error;
    auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    send_query(G()->net_query_creator().create(create_storer(
        telegram_api::channels_deleteHistory(std::move(input_channel), max_message_id.get_server_message_id().get()))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_deleteHistory>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG_IF(ERROR, !allow_error_ && !result)
        << "Delete history in " << channel_id_ << " up to " << max_message_id_ << " failed";

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->contacts_manager_->on_get_channel_error(channel_id_, status, "DeleteChannelHistoryQuery")) {
      LOG(ERROR) << "Receive error for deleteChannelHistory: " << status;
    }
    promise_.set_error(std::move(status));
  }
};

class DeleteUserHistoryQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  UserId user_id_;

  void send_request() {
    auto input_channel = td->contacts_manager_->get_input_channel(channel_id_);
    if (input_channel == nullptr) {
      return promise_.set_error(Status::Error(3, "Chat is not accessible"));
    }
    auto input_user = td->contacts_manager_->get_input_user(user_id_);
    if (input_user == nullptr) {
      return promise_.set_error(Status::Error(3, "Usat is not accessible"));
    }

    LOG(INFO) << "Delete all messages from " << user_id_ << " in " << channel_id_;

    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::channels_deleteUserHistory(std::move(input_channel), std::move(input_user)))));
  }

 public:
  explicit DeleteUserHistoryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, UserId user_id) {
    channel_id_ = channel_id;
    user_id_ = user_id;

    send_request();
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_deleteUserHistory>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto affected_history = result_ptr.move_as_ok();
    CHECK(affected_history->get_id() == telegram_api::messages_affectedHistory::ID);

    if (affected_history->pts_count_ > 0) {
      td->messages_manager_->add_pending_channel_update(DialogId(channel_id_), make_tl_object<dummyUpdate>(),
                                                        affected_history->pts_, affected_history->pts_count_,
                                                        "delete user history query");
    }

    if (affected_history->offset_ > 0) {
      send_request();
      return;
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->contacts_manager_->on_get_channel_error(channel_id_, status, "DeleteUserHistoryQuery");
    promise_.set_error(std::move(status));
  }
};

class ReadAllMentionsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

  void send_request() {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id_, AccessRights::Read);
    if (input_peer == nullptr) {
      return promise_.set_error(Status::Error(3, "Chat is not accessible"));
    }

    LOG(INFO) << "Read all mentions in " << dialog_id_;

    send_query(
        G()->net_query_creator().create(create_storer(telegram_api::messages_readMentions(std::move(input_peer)))));
  }

 public:
  explicit ReadAllMentionsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id) {
    dialog_id_ = dialog_id;

    send_request();
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_readMentions>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto affected_history = result_ptr.move_as_ok();
    CHECK(affected_history->get_id() == telegram_api::messages_affectedHistory::ID);

    if (affected_history->pts_count_ > 0) {
      if (dialog_id_.get_type() == DialogType::Channel) {
        LOG(ERROR) << "Receive pts_count " << affected_history->pts_count_ << " in result of ReadAllMentionsQuery in "
                   << dialog_id_;
        td->updates_manager_->get_difference("Wrong messages_readMentions result");
      } else {
        td->messages_manager_->add_pending_update(make_tl_object<dummyUpdate>(), affected_history->pts_,
                                                  affected_history->pts_count_, false, "read all mentions query");
      }
    }

    if (affected_history->offset_ > 0) {
      send_request();
      return;
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "ReadAllMentionsQuery");
    promise_.set_error(std::move(status));
  }
};

class SendSecretMessageActor : public NetActor {
  int64 random_id_;

 public:
  void send(DialogId dialog_id, int64 reply_to_random_id, int32 ttl, const string &message, SecretInputMedia media,
            vector<tl_object_ptr<secret_api::MessageEntity>> &&entities, UserId via_bot_user_id, int64 media_album_id,
            int64 random_id) {
    if (false && !media.empty()) {
      td->messages_manager_->on_send_secret_message_error(random_id, Status::Error(400, "FILE_PART_1_MISSING"), Auto());
      return;
    }

    CHECK(dialog_id.get_type() == DialogType::SecretChat);
    random_id_ = random_id;

    int32 flags = 0;
    if (reply_to_random_id != 0) {
      flags |= secret_api::decryptedMessage::REPLY_TO_RANDOM_ID_MASK;
    }
    if (via_bot_user_id.is_valid()) {
      flags |= secret_api::decryptedMessage::VIA_BOT_NAME_MASK;
    }
    if (!media.empty()) {
      flags |= secret_api::decryptedMessage::MEDIA_MASK;
    }
    if (!entities.empty()) {
      flags |= secret_api::decryptedMessage::ENTITIES_MASK;
    }
    if (media_album_id != 0) {
      CHECK(media_album_id < 0);
      flags |= secret_api::decryptedMessage::GROUPED_ID_MASK;
    }

    send_closure(G()->secret_chats_manager(), &SecretChatsManager::send_message, dialog_id.get_secret_chat_id(),
                 make_tl_object<secret_api::decryptedMessage>(
                     flags, random_id, ttl, message, std::move(media.decrypted_media_), std::move(entities),
                     td->contacts_manager_->get_user_username(via_bot_user_id), reply_to_random_id, -media_album_id),
                 std::move(media.input_file_),
                 PromiseCreator::event(self_closure(this, &SendSecretMessageActor::done)));
  }

  void done() {
    stop();
  }
};

class SendMessageActor : public NetActorOnce {
  int64 random_id_;
  DialogId dialog_id_;

 public:
  void send(int32 flags, DialogId dialog_id, MessageId reply_to_message_id,
            tl_object_ptr<telegram_api::ReplyMarkup> &&reply_markup,
            vector<tl_object_ptr<telegram_api::MessageEntity>> &&entities, const string &message, int64 random_id,
            NetQueryRef *send_query_ref, uint64 sequence_dispatcher_id) {
    random_id_ = random_id;
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      on_error(0, Status::Error(400, "Have no write access to the chat"));
      return;
    }

    if (!entities.empty()) {
      flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_ENTITIES;
    }

    auto query = G()->net_query_creator().create(create_storer(telegram_api::messages_sendMessage(
        flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, std::move(input_peer),
        reply_to_message_id.get_server_message_id().get(), message, random_id, std::move(reply_markup),
        std::move(entities))));
    if (G()->shared_config().get_option_boolean("use_quick_ack")) {
      query->quick_ack_promise_ = PromiseCreator::lambda(
          [random_id](Unit) {
            send_closure(G()->messages_manager(), &MessagesManager::on_send_message_get_quick_ack, random_id);
          },
          PromiseCreator::Ignore());
    }
    *send_query_ref = query.get_weak();
    query->debug("send to MessagesManager::MultiSequenceDispatcher");
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_dispatcher_id);
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_sendMessage>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for sendMessage for " << random_id_ << ": " << to_string(ptr);

    auto constructor_id = ptr->get_id();
    if (constructor_id != telegram_api::updateShortSentMessage::ID) {
      td->messages_manager_->check_send_message_result(random_id_, dialog_id_, ptr.get(), "SendMessage");
      return td->updates_manager_->on_get_updates(std::move(ptr));
    }
    auto sent_message = move_tl_object_as<telegram_api::updateShortSentMessage>(ptr);
    td->messages_manager_->on_update_sent_text_message(random_id_, std::move(sent_message->media_),
                                                       std::move(sent_message->entities_));

    auto message_id = MessageId(ServerMessageId(sent_message->id_));
    if (dialog_id_.get_type() == DialogType::Channel) {
      td->messages_manager_->add_pending_channel_update(
          dialog_id_, make_tl_object<updateSentMessage>(random_id_, message_id, sent_message->date_),
          sent_message->pts_, sent_message->pts_count_, "send message actor");
      return;
    }

    td->messages_manager_->add_pending_update(
        make_tl_object<updateSentMessage>(random_id_, message_id, sent_message->date_), sent_message->pts_,
        sent_message->pts_count_, false, "send message actor");
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for sendMessage: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, message will be re-sent
      return;
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SendMessageActor");
    td->messages_manager_->on_send_message_fail(random_id_, std::move(status));
  }
};

class StartBotQuery : public Td::ResultHandler {
  int64 random_id_;
  DialogId dialog_id_;

 public:
  NetQueryRef send(tl_object_ptr<telegram_api::InputUser> bot_input_user, DialogId dialog_id,
                   tl_object_ptr<telegram_api::InputPeer> input_peer, const string &parameter, int64 random_id) {
    CHECK(bot_input_user != nullptr);
    CHECK(input_peer != nullptr);
    random_id_ = random_id;
    dialog_id_ = dialog_id;

    auto query = G()->net_query_creator().create(create_storer(
        telegram_api::messages_startBot(std::move(bot_input_user), std::move(input_peer), random_id, parameter)));
    if (G()->shared_config().get_option_boolean("use_quick_ack")) {
      query->quick_ack_promise_ = PromiseCreator::lambda(
          [random_id](Unit) {
            send_closure(G()->messages_manager(), &MessagesManager::on_send_message_get_quick_ack, random_id);
          },
          PromiseCreator::Ignore());
    }
    auto send_query_ref = query.get_weak();
    send_query(std::move(query));
    return send_query_ref;
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_startBot>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for startBot for " << random_id_ << ": " << to_string(ptr);
    // Result may contain messageActionChatAddUser
    // td->messages_manager_->check_send_message_result(random_id_, dialog_id_, ptr.get(), "StartBot");
    td->updates_manager_->on_get_updates(std::move(ptr));
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for startBot: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, message should be re-sent
      return;
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "StartBotQuery");
    td->messages_manager_->on_send_message_fail(random_id_, std::move(status));
  }
};

class SendInlineBotResultQuery : public Td::ResultHandler {
  int64 random_id_;
  DialogId dialog_id_;

 public:
  NetQueryRef send(int32 flags, DialogId dialog_id, MessageId reply_to_message_id, int64 random_id, int64 query_id,
                   const string &result_id) {
    random_id_ = random_id;
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    CHECK(input_peer != nullptr);

    auto query = G()->net_query_creator().create(create_storer(telegram_api::messages_sendInlineBotResult(
        flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, std::move(input_peer),
        reply_to_message_id.get_server_message_id().get(), random_id, query_id, result_id)));
    auto send_query_ref = query.get_weak();
    send_query(std::move(query));
    return send_query_ref;
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_sendInlineBotResult>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for sendInlineBotResult for " << random_id_ << ": " << to_string(ptr);
    td->messages_manager_->check_send_message_result(random_id_, dialog_id_, ptr.get(), "SendInlineBotResult");
    td->updates_manager_->on_get_updates(std::move(ptr));
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for sendInlineBotResult: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, message will be re-sent
      return;
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SendInlineBotResultQuery");
    td->messages_manager_->on_send_message_fail(random_id_, std::move(status));
  }
};

class SendMultiMediaActor : public NetActorOnce {
  vector<int64> random_ids_;
  DialogId dialog_id_;

 public:
  void send(int32 flags, DialogId dialog_id, MessageId reply_to_message_id,
            vector<tl_object_ptr<telegram_api::inputSingleMedia>> &&input_single_media, uint64 sequence_dispatcher_id) {
    for (auto &single_media : input_single_media) {
      random_ids_.push_back(single_media->random_id_);
    }
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      on_error(0, Status::Error(400, "Have no write access to the chat"));
      return;
    }

    auto query = G()->net_query_creator().create(create_storer(telegram_api::messages_sendMultiMedia(
        flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, std::move(input_peer),
        reply_to_message_id.get_server_message_id().get(), std::move(input_single_media))));
    if (G()->shared_config().get_option_boolean("use_quick_ack")) {
      query->quick_ack_promise_ = PromiseCreator::lambda(
          [random_ids = random_ids_](Unit) {
            for (auto random_id : random_ids) {
              send_closure(G()->messages_manager(), &MessagesManager::on_send_message_get_quick_ack, random_id);
            }
          },
          PromiseCreator::Ignore());
    }
    query->debug("send to MessagesManager::MultiSequenceDispatcher");
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_dispatcher_id);
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_sendMultiMedia>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for sendMultiMedia for " << format::as_array(random_ids_) << ": " << to_string(ptr);

    auto sent_random_ids = td->updates_manager_->get_sent_messages_random_ids(ptr.get());
    bool is_result_wrong = false;
    auto sent_random_ids_size = sent_random_ids.size();
    for (auto &random_id : random_ids_) {
      auto it = sent_random_ids.find(random_id);
      if (it == sent_random_ids.end()) {
        if (random_ids_.size() == 1) {
          is_result_wrong = true;
        }
        td->messages_manager_->on_send_message_fail(random_id, Status::Error(400, "Message was not sent"));
      } else {
        sent_random_ids.erase(it);
      }
    }
    if (!sent_random_ids.empty()) {
      is_result_wrong = true;
    }
    if (!is_result_wrong) {
      auto sent_messages = td->updates_manager_->get_new_messages(ptr.get());
      if (sent_random_ids_size != sent_messages.size()) {
        is_result_wrong = true;
      }
      for (auto &sent_message : sent_messages) {
        if (td->messages_manager_->get_message_dialog_id(*sent_message) != dialog_id_) {
          is_result_wrong = true;
        }
      }
    }
    if (is_result_wrong) {
      LOG(ERROR) << "Receive wrong result for sendMultiMedia with random_ids " << format::as_array(random_ids_)
                 << " to " << dialog_id_ << ": " << oneline(to_string(ptr));
      td->updates_manager_->schedule_get_difference("Wrong sendMultiMedia result");
    }

    td->updates_manager_->on_get_updates(std::move(ptr));
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for sendMultiMedia: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, message will be re-sent
      return;
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SendMultiMediaActor");
    for (auto &random_id : random_ids_) {
      td->messages_manager_->on_send_message_fail(random_id, status.clone());
    }
  }
};

class SendMediaActor : public NetActorOnce {
  int64 random_id_;
  FileId file_id_;
  FileId thumbnail_file_id_;
  DialogId dialog_id_;

 public:
  void send(FileId file_id, FileId thumbnail_file_id, int32 flags, DialogId dialog_id, MessageId reply_to_message_id,
            tl_object_ptr<telegram_api::ReplyMarkup> &&reply_markup,
            vector<tl_object_ptr<telegram_api::MessageEntity>> &&entities, const string &message,
            tl_object_ptr<telegram_api::InputMedia> &&input_media, int64 random_id, NetQueryRef *send_query_ref,
            uint64 sequence_dispatcher_id) {
    random_id_ = random_id;
    file_id_ = file_id;
    thumbnail_file_id_ = thumbnail_file_id;
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      on_error(0, Status::Error(400, "Have no write access to the chat"));
      return;
    }
    if (!entities.empty()) {
      flags |= telegram_api::messages_sendMedia::ENTITIES_MASK;
    }

    telegram_api::messages_sendMedia request(flags, false /*ignored*/, false /*ignored*/, false /*ignored*/,
                                             std::move(input_peer), reply_to_message_id.get_server_message_id().get(),
                                             std::move(input_media), message, random_id, std::move(reply_markup),
                                             std::move(entities));
    LOG(INFO) << "Send media: " << to_string(request);
    auto query = G()->net_query_creator().create(create_storer(request));
    if (G()->shared_config().get_option_boolean("use_quick_ack")) {
      query->quick_ack_promise_ = PromiseCreator::lambda(
          [random_id](Unit) {
            send_closure(G()->messages_manager(), &MessagesManager::on_send_message_get_quick_ack, random_id);
          },
          PromiseCreator::Ignore());
    }
    *send_query_ref = query.get_weak();
    query->debug("send to MessagesManager::MultiSequenceDispatcher");
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_dispatcher_id);
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_sendMedia>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    if (thumbnail_file_id_.is_valid()) {
      // always delete partial remote location for the thumbnail, because it can't be reused anyway
      // TODO delete it only in the case it can't be merged with file thumbnail
      td->file_manager_->delete_partial_remote_location(thumbnail_file_id_);
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for sendMedia for " << random_id_ << ": " << to_string(ptr);
    td->messages_manager_->check_send_message_result(random_id_, dialog_id_, ptr.get(), "SendMedia");
    td->updates_manager_->on_get_updates(std::move(ptr));
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for sendMedia: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, message will be re-sent
      return;
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SendMediaActor");
    if (file_id_.is_valid()) {
      if (begins_with(status.message(), "FILE_PART_") && ends_with(status.message(), "_MISSING")) {
        td->messages_manager_->on_send_message_file_part_missing(random_id_,
                                                                 to_integer<int32>(status.message().substr(10)));
        return;
      } else {
        if (status.code() != 429 && status.code() < 500 && !G()->close_flag()) {
          td->file_manager_->delete_partial_remote_location(file_id_);
        }
      }
    }
    if (thumbnail_file_id_.is_valid()) {
      // always delete partial remote location for the thumbnail, because it can't be reused anyway
      td->file_manager_->delete_partial_remote_location(thumbnail_file_id_);
    }
    td->messages_manager_->on_send_message_fail(random_id_, std::move(status));
  }
};

class UploadMediaQuery : public Td::ResultHandler {
  DialogId dialog_id_;
  MessageId message_id_;
  FileId file_id_;
  FileId thumbnail_file_id_;

 public:
  void send(DialogId dialog_id, MessageId message_id, FileId file_id, FileId thumbnail_file_id,
            tl_object_ptr<telegram_api::InputMedia> &&input_media) {
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      on_error(0, Status::Error(400, "Have no write access to the chat"));
      return;
    }
    CHECK(input_media != nullptr);

    dialog_id_ = dialog_id;
    message_id_ = message_id;
    file_id_ = file_id;
    thumbnail_file_id_ = thumbnail_file_id;

    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_uploadMedia(std::move(input_peer), std::move(input_media)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_uploadMedia>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for uploadMedia: " << to_string(ptr);
    td->messages_manager_->on_upload_message_media_success(dialog_id_, message_id_, std::move(ptr));
  }

  void on_error(uint64 id, Status status) override {
    LOG(WARNING) << "Receive error for uploadMedia: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, message will be re-sent
      return;
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "UploadMediaQuery");
    if (file_id_.is_valid()) {
      if (begins_with(status.message(), "FILE_PART_") && ends_with(status.message(), "_MISSING")) {
        td->messages_manager_->on_upload_message_media_file_part_missing(
            dialog_id_, message_id_, to_integer<int32>(status.message().substr(10)));
        return;
      } else {
        if (status.code() != 429 && status.code() < 500 && !G()->close_flag()) {
          td->file_manager_->delete_partial_remote_location(file_id_);
        }
      }
    }
    td->messages_manager_->on_upload_message_media_fail(dialog_id_, message_id_, std::move(status));
  }
};

class EditMessageActor : public NetActorOnce {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit EditMessageActor(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int32 flags, DialogId dialog_id, MessageId message_id, const string &message,
            vector<tl_object_ptr<telegram_api::MessageEntity>> &&entities,
            tl_object_ptr<telegram_api::InputGeoPoint> &&input_geo_point,
            tl_object_ptr<telegram_api::ReplyMarkup> &&reply_markup, uint64 sequence_dispatcher_id) {
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Edit);
    if (input_peer == nullptr) {
      on_error(0, Status::Error(400, "Can't access the chat"));
      return;
    }

    if (reply_markup != nullptr) {
      flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_REPLY_MARKUP;
    }
    if (!entities.empty()) {
      flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_ENTITIES;
    }
    if (!message.empty()) {
      flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_MESSAGE;
    }
    if (input_geo_point != nullptr) {
      flags |= telegram_api::messages_editMessage::GEO_POINT_MASK;
    }
    LOG(DEBUG) << "Edit message with flags " << flags;

    auto query = G()->net_query_creator().create(create_storer(telegram_api::messages_editMessage(
        flags, false /*ignored*/, false /*ignored*/, std::move(input_peer), message_id.get_server_message_id().get(),
        message, std::move(reply_markup), std::move(entities), std::move(input_geo_point))));

    query->debug("send to MessagesManager::MultiSequenceDispatcher");
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_dispatcher_id);
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_editMessage>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for editMessage: " << to_string(ptr);
    if (ptr->get_id() == telegram_api::updateShortSentMessage::ID) {
      LOG(ERROR) << "Receive updateShortSentMessage in edit message";
      return on_error(id, Status::Error(500, "Unsupported result was returned from the server"));
    }

    td->updates_manager_->on_get_updates(std::move(ptr));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for editMessage: " << status;
    if (!td->auth_manager_->is_bot() && status.message() == "MESSAGE_NOT_MODIFIED") {
      return promise_.set_value(Unit());
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "EditMessageActor");
    promise_.set_error(std::move(status));
  }
};

class EditInlineMessageQuery : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit EditInlineMessageQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int32 flags, tl_object_ptr<telegram_api::inputBotInlineMessageID> input_bot_inline_message_id,
            const string &message, vector<tl_object_ptr<telegram_api::MessageEntity>> &&entities,
            tl_object_ptr<telegram_api::InputGeoPoint> &&input_geo_point,
            tl_object_ptr<telegram_api::ReplyMarkup> &&reply_markup) {
    CHECK(input_bot_inline_message_id != nullptr);

    if (reply_markup != nullptr) {
      flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_REPLY_MARKUP;
    }
    if (!entities.empty()) {
      flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_ENTITIES;
    }
    if (!message.empty()) {
      flags |= MessagesManager::SEND_MESSAGE_FLAG_HAS_MESSAGE;
    }
    if (input_geo_point != nullptr) {
      flags |= telegram_api::messages_editInlineBotMessage::GEO_POINT_MASK;
    }
    LOG(DEBUG) << "Edit inline message with flags " << flags;

    auto dc_id = DcId::internal(input_bot_inline_message_id->dc_id_);
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_editInlineBotMessage(
            flags, false /*ignored*/, false /*ignored*/, std::move(input_bot_inline_message_id), message,
            std::move(reply_markup), std::move(entities), std::move(input_geo_point))),
        dc_id));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_editInlineBotMessage>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    LOG_IF(ERROR, !result_ptr.ok()) << "Receive false in result of editInlineMessage";

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for editInlineMessage: " << status;
    promise_.set_error(std::move(status));
  }
};

class SetGameScoreActor : public NetActorOnce {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit SetGameScoreActor(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId message_id, bool edit_message,
            tl_object_ptr<telegram_api::InputUser> input_user, int32 score, bool force, uint64 sequence_dispatcher_id) {
    int32 flags = 0;
    if (edit_message) {
      flags |= telegram_api::messages_setGameScore::EDIT_MESSAGE_MASK;
    }
    if (force) {
      flags |= telegram_api::messages_setGameScore::FORCE_MASK;
    }

    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Edit);
    if (input_peer == nullptr) {
      on_error(0, Status::Error(400, "Can't access the chat"));
      return;
    }

    CHECK(input_user != nullptr);
    auto query = G()->net_query_creator().create(create_storer(
        telegram_api::messages_setGameScore(flags, false /*ignored*/, false /*ignored*/, std::move(input_peer),
                                            message_id.get_server_message_id().get(), std::move(input_user), score)));

    LOG(INFO) << "Set game score to " << score;

    query->debug("send to MessagesManager::MultiSequenceDispatcher");
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_dispatcher_id);
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_setGameScore>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for setGameScore: " << to_string(ptr);
    td->updates_manager_->on_get_updates(std::move(ptr));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for setGameScore: " << status;
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SetGameScoreActor");
    promise_.set_error(std::move(status));
  }
};

class SetInlineGameScoreQuery : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit SetInlineGameScoreQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(tl_object_ptr<telegram_api::inputBotInlineMessageID> input_bot_inline_message_id, bool edit_message,
            tl_object_ptr<telegram_api::InputUser> input_user, int32 score, bool force) {
    CHECK(input_bot_inline_message_id != nullptr);
    CHECK(input_user != nullptr);

    int32 flags = 0;
    if (edit_message) {
      flags |= telegram_api::messages_setInlineGameScore::EDIT_MESSAGE_MASK;
    }
    if (force) {
      flags |= telegram_api::messages_setInlineGameScore::FORCE_MASK;
    }

    LOG(INFO) << "Set inline game score to " << score;
    auto dc_id = DcId::internal(input_bot_inline_message_id->dc_id_);
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_setInlineGameScore(flags, false /*ignored*/, false /*ignored*/,
                                                                std::move(input_bot_inline_message_id),
                                                                std::move(input_user), score)),
        dc_id));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_setInlineGameScore>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    LOG_IF(ERROR, !result_ptr.ok()) << "Receive false in result of setInlineGameScore";

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for setInlineGameScore: " << status;
    promise_.set_error(std::move(status));
  }
};

class GetGameHighScoresQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  int64 random_id_;

 public:
  explicit GetGameHighScoresQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId message_id, tl_object_ptr<telegram_api::InputUser> input_user,
            int64 random_id) {
    dialog_id_ = dialog_id;
    random_id_ = random_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);

    CHECK(input_user != nullptr);
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_getGameHighScores(
        std::move(input_peer), message_id.get_server_message_id().get(), std::move(input_user)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getGameHighScores>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    td->messages_manager_->on_get_game_high_scores(random_id_, result_ptr.move_as_ok());
    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for getGameHighScores: " << status;
    td->messages_manager_->on_get_game_high_scores(random_id_, nullptr);
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetGameHighScoresQuery");
    promise_.set_error(std::move(status));
  }
};

class GetInlineGameHighScoresQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  int64 random_id_;

 public:
  explicit GetInlineGameHighScoresQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(tl_object_ptr<telegram_api::inputBotInlineMessageID> input_bot_inline_message_id,
            tl_object_ptr<telegram_api::InputUser> input_user, int64 random_id) {
    CHECK(input_bot_inline_message_id != nullptr);
    CHECK(input_user != nullptr);

    random_id_ = random_id;

    auto dc_id = DcId::internal(input_bot_inline_message_id->dc_id_);
    send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_getInlineGameHighScores(
                                                   std::move(input_bot_inline_message_id), std::move(input_user))),
                                               dc_id));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getInlineGameHighScores>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    td->messages_manager_->on_get_game_high_scores(random_id_, result_ptr.move_as_ok());
    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for getInlineGameHighScores: " << status;
    td->messages_manager_->on_get_game_high_scores(random_id_, nullptr);
    promise_.set_error(std::move(status));
  }
};

class ForwardMessagesActor : public NetActorOnce {
  Promise<Unit> promise_;
  vector<int64> random_ids_;
  DialogId to_dialog_id_;

 public:
  explicit ForwardMessagesActor(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int32 flags, DialogId to_dialog_id, DialogId from_dialog_id, const vector<MessageId> &message_ids,
            vector<int64> &&random_ids, uint64 sequence_dispatcher_id) {
    LOG(INFO) << "Forward " << format::as_array(message_ids) << " from " << from_dialog_id << " to " << to_dialog_id;

    random_ids_ = random_ids;
    to_dialog_id_ = to_dialog_id;

    auto to_input_peer = td->messages_manager_->get_input_peer(to_dialog_id, AccessRights::Write);
    if (to_input_peer == nullptr) {
      on_error(0, Status::Error(400, "Have no write access to the chat"));
      return;
    }

    auto from_input_peer = td->messages_manager_->get_input_peer(from_dialog_id, AccessRights::Read);
    if (from_input_peer == nullptr) {
      on_error(0, Status::Error(400, "Can't access the chat to forward messages from"));
      return;
    }

    auto query = G()->net_query_creator().create(create_storer(telegram_api::messages_forwardMessages(
        flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, std::move(from_input_peer),
        MessagesManager::get_server_message_ids(message_ids), std::move(random_ids), std::move(to_input_peer))));
    if (G()->shared_config().get_option_boolean("use_quick_ack")) {
      query->quick_ack_promise_ = PromiseCreator::lambda(
          [random_ids = random_ids_](Unit) {
            for (auto random_id : random_ids) {
              send_closure(G()->messages_manager(), &MessagesManager::on_send_message_get_quick_ack, random_id);
            }
          },
          PromiseCreator::Ignore());
    }
    send_closure(td->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_dispatcher_id);
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_forwardMessages>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for forwardMessages for " << format::as_array(random_ids_) << ": " << to_string(ptr);
    auto sent_random_ids = td->updates_manager_->get_sent_messages_random_ids(ptr.get());
    bool is_result_wrong = false;
    auto sent_random_ids_size = sent_random_ids.size();
    for (auto &random_id : random_ids_) {
      auto it = sent_random_ids.find(random_id);
      if (it == sent_random_ids.end()) {
        if (random_ids_.size() == 1) {
          is_result_wrong = true;
        }
        td->messages_manager_->on_send_message_fail(random_id, Status::Error(400, "Message was not forwarded"));
      } else {
        sent_random_ids.erase(it);
      }
    }
    if (!sent_random_ids.empty()) {
      is_result_wrong = true;
    }
    if (!is_result_wrong) {
      auto sent_messages = td->updates_manager_->get_new_messages(ptr.get());
      if (sent_random_ids_size != sent_messages.size()) {
        is_result_wrong = true;
      }
      for (auto &sent_message : sent_messages) {
        if (td->messages_manager_->get_message_dialog_id(*sent_message) != to_dialog_id_) {
          is_result_wrong = true;
        }
      }
    }
    if (is_result_wrong) {
      LOG(ERROR) << "Receive wrong result for forwarding messages with random_ids " << format::as_array(random_ids_)
                 << " to " << to_dialog_id_ << ": " << oneline(to_string(ptr));
      td->updates_manager_->schedule_get_difference("Wrong forwardMessages result");
    }

    td->updates_manager_->on_get_updates(std::move(ptr));
    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for forward messages: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, messages should be re-sent
      return;
    }
    // no on_get_dialog_error call, because two dialogs are involved
    for (auto &random_id : random_ids_) {
      td->messages_manager_->on_send_message_fail(random_id, status.clone());
    }
    promise_.set_error(std::move(status));
  }
};

class SendScreenshotNotificationQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  int64 random_id_;
  DialogId dialog_id_;

 public:
  explicit SendScreenshotNotificationQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, int64 random_id) {
    random_id_ = random_id;
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    CHECK(input_peer != nullptr);

    auto query = G()->net_query_creator().create(
        create_storer(telegram_api::messages_sendScreenshotNotification(std::move(input_peer), 0, random_id)));
    send_query(std::move(query));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_sendScreenshotNotification>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for SendScreenshotNotificationQuery for " << random_id_ << ": " << to_string(ptr);
    td->messages_manager_->check_send_message_result(random_id_, dialog_id_, ptr.get(),
                                                     "SendScreenshotNotificationQuery");
    td->updates_manager_->on_get_updates(std::move(ptr));
    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for SendScreenshotNotificationQuery: " << status;
    if (G()->close_flag() && G()->parameters().use_message_db) {
      // do not send error, messages should be re-sent
      return;
    }
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SendScreenshotNotificationQuery");
    td->messages_manager_->on_send_message_fail(random_id_, status.clone());
    promise_.set_error(std::move(status));
  }
};

class SetTypingQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit SetTypingQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  NetQueryRef send(DialogId dialog_id, tl_object_ptr<telegram_api::SendMessageAction> &&action) {
    dialog_id_ = dialog_id;
    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    CHECK(input_peer != nullptr);

    auto net_query = G()->net_query_creator().create(
        create_storer(telegram_api::messages_setTyping(std::move(input_peer), std::move(action))));
    auto result = net_query.get_weak();
    send_query(std::move(net_query));
    return result;
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_setTyping>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    // ignore result

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (status.code() == NetQuery::Cancelled) {
      return promise_.set_value(Unit());
    }

    LOG(INFO) << "Receive error for set typing: " << status;
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "SetTypingQuery");
    promise_.set_error(std::move(status));
  }
};

class DeleteMessagesQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  int32 query_count_;

 public:
  explicit DeleteMessagesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<MessageId> &&message_ids, bool revoke) {
    LOG(INFO) << "Send deleteMessagesQuery to delete " << format::as_array(message_ids);
    int32 flags = 0;
    if (revoke) {
      flags |= telegram_api::messages_deleteMessages::REVOKE_MASK;
    }

    query_count_ = 0;
    auto server_message_ids = MessagesManager::get_server_message_ids(message_ids);
    const size_t MAX_SLICE_SIZE = 100;
    for (size_t i = 0; i < server_message_ids.size(); i += MAX_SLICE_SIZE) {
      auto end_i = i + MAX_SLICE_SIZE;
      auto end = end_i < server_message_ids.size() ? server_message_ids.begin() + end_i : server_message_ids.end();
      vector<int32> slice(server_message_ids.begin() + i, end);

      query_count_++;
      send_query(G()->net_query_creator().create(
          create_storer(telegram_api::messages_deleteMessages(flags, false /*ignored*/, std::move(slice)))));
    }
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_deleteMessages>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto affected_messages = result_ptr.move_as_ok();
    CHECK(affected_messages->get_id() == telegram_api::messages_affectedMessages::ID);

    if (affected_messages->pts_count_ > 0) {
      td->messages_manager_->add_pending_update(make_tl_object<dummyUpdate>(), affected_messages->pts_,
                                                affected_messages->pts_count_, false, "delete messages query");
    }
    if (--query_count_ == 0) {
      promise_.set_value(Unit());
    }
  }

  void on_error(uint64 id, Status status) override {
    LOG(ERROR) << "Receive error for delete messages: " << status;
    promise_.set_error(std::move(status));
  }
};

class DeleteChannelMessagesQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  int32 query_count_;
  ChannelId channel_id_;

 public:
  explicit DeleteChannelMessagesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, vector<MessageId> &&message_ids) {
    channel_id_ = channel_id;
    LOG(INFO) << "Send deleteChannelMessagesQuery to delete " << format::as_array(message_ids) << " in the "
              << channel_id;

    query_count_ = 0;
    auto server_message_ids = MessagesManager::get_server_message_ids(message_ids);
    const size_t MAX_SLICE_SIZE = 100;
    for (size_t i = 0; i < server_message_ids.size(); i += MAX_SLICE_SIZE) {
      auto end_i = i + MAX_SLICE_SIZE;
      auto end = end_i < server_message_ids.size() ? server_message_ids.begin() + end_i : server_message_ids.end();
      vector<int32> slice(server_message_ids.begin() + i, end);

      query_count_++;
      auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
      CHECK(input_channel != nullptr);
      send_query(G()->net_query_creator().create(
          create_storer(telegram_api::channels_deleteMessages(std::move(input_channel), std::move(slice)))));
    }
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_deleteMessages>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto affected_messages = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for deleteChannelMessages: " << to_string(affected_messages);
    CHECK(affected_messages->get_id() == telegram_api::messages_affectedMessages::ID);

    if (affected_messages->pts_count_ > 0) {
      td->messages_manager_->add_pending_channel_update(DialogId(channel_id_), make_tl_object<dummyUpdate>(),
                                                        affected_messages->pts_, affected_messages->pts_count_,
                                                        "DeleteChannelMessagesQuery");
    }
    if (--query_count_ == 0) {
      promise_.set_value(Unit());
    }
  }

  void on_error(uint64 id, Status status) override {
    if (!td->contacts_manager_->on_get_channel_error(channel_id_, status, "DeleteChannelMessagesQuery")) {
      LOG(ERROR) << "Receive error for delete channel messages: " << status;
    }
    promise_.set_error(std::move(status));
  }
};

class GetNotifySettingsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  NotificationSettingsScope scope_;

 public:
  explicit GetNotifySettingsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(NotificationSettingsScope scope) {
    scope_ = scope;
    auto input_notify_peer = td->messages_manager_->get_input_notify_peer(scope);
    CHECK(input_notify_peer != nullptr);
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::account_getNotifySettings(std::move(input_notify_peer)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::account_getNotifySettings>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    td->messages_manager_->on_update_notify_settings(scope_, std::move(ptr));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class UpdateNotifySettingsQuery : public Td::ResultHandler {
  NotificationSettingsScope scope_;

 public:
  void send(NotificationSettingsScope scope, const NotificationSettings &new_settings) {
    auto input_notify_peer = td->messages_manager_->get_input_notify_peer(scope);
    if (input_notify_peer == nullptr) {
      return;
    }
    int32 flags = 0;
    if (new_settings.show_preview) {
      flags |= telegram_api::inputPeerNotifySettings::SHOW_PREVIEWS_MASK;
    }
    if (new_settings.silent_send_message) {
      flags |= telegram_api::inputPeerNotifySettings::SILENT_MASK;
    }
    send_query(G()->net_query_creator().create(create_storer(telegram_api::account_updateNotifySettings(
        std::move(input_notify_peer),
        make_tl_object<telegram_api::inputPeerNotifySettings>(flags, false /*ignored*/, false /*ignored*/,
                                                              new_settings.mute_until, new_settings.sound)))));
    scope_ = scope;
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::account_updateNotifySettings>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      return on_error(id, Status::Error(400, "Receive false as result"));
    }
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for set notification settings: " << status;
    status.ignore();

    if (!td->auth_manager_->is_bot() && td->messages_manager_->get_input_notify_peer(scope_) != nullptr) {
      // trying to repair notification settings for this scope
      td->create_handler<GetNotifySettingsQuery>(Promise<>())->send(scope_);
    }
  }
};

class ResetNotifySettingsQuery : public Td::ResultHandler {
 public:
  void send() {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::account_resetNotifySettings())));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::account_resetNotifySettings>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      return on_error(id, Status::Error(400, "Receive false as result"));
    }
  }

  void on_error(uint64 id, Status status) override {
    LOG(WARNING) << "Receive error for reset notification settings: " << status;
    status.ignore();
  }
};

class GetPeerSettingsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit GetPeerSettingsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id) {
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);

    send_query(
        G()->net_query_creator().create(create_storer(telegram_api::messages_getPeerSettings(std::move(input_peer)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_getPeerSettings>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    td->messages_manager_->on_get_peer_settings(dialog_id_, result_ptr.move_as_ok());

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for get peer settings: " << status;
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetPeerSettingsQuery");
    promise_.set_error(std::move(status));
  }
};

class UpdatePeerSettingsQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit UpdatePeerSettingsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, bool is_spam_dialog) {
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);

    if (is_spam_dialog) {
      send_query(
          G()->net_query_creator().create(create_storer(telegram_api::messages_reportSpam(std::move(input_peer)))));
    } else {
      send_query(
          G()->net_query_creator().create(create_storer(telegram_api::messages_hideReportSpam(std::move(input_peer)))));
    }
  }

  void on_result(uint64 id, BufferSlice packet) override {
    static_assert(std::is_same<telegram_api::messages_reportSpam::ReturnType,
                               telegram_api::messages_hideReportSpam::ReturnType>::value,
                  "");
    auto result_ptr = fetch_result<telegram_api::messages_reportSpam>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    td->messages_manager_->on_get_peer_settings(dialog_id_,
                                                make_tl_object<telegram_api::peerSettings>(0, false /*ignored*/));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for update peer settings: " << status;
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "UpdatePeerSettingsQuery");
    promise_.set_error(std::move(status));
  }
};

class ReportEncryptedSpamQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit ReportEncryptedSpamQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id) {
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_encrypted_chat(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);
    LOG(INFO) << "Report spam in " << to_string(input_peer);

    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::messages_reportEncryptedSpam(std::move(input_peer)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::messages_reportEncryptedSpam>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    td->messages_manager_->on_get_peer_settings(dialog_id_,
                                                make_tl_object<telegram_api::peerSettings>(0, false /*ignored*/));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for report encrypted spam: " << status;
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "ReportEncryptedSpamQuery");
    promise_.set_error(std::move(status));
  }
};

class ReportPeerQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit ReportPeerQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, tl_object_ptr<telegram_api::ReportReason> &&report_reason,
            const vector<MessageId> &message_ids) {
    dialog_id_ = dialog_id;

    auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);

    if (message_ids.empty()) {
      send_query(G()->net_query_creator().create(
          create_storer(telegram_api::account_reportPeer(std::move(input_peer), std::move(report_reason)))));
    } else {
      send_query(G()->net_query_creator().create(create_storer(telegram_api::messages_report(
          std::move(input_peer), MessagesManager::get_server_message_ids(message_ids), std::move(report_reason)))));
    }
  }

  void on_result(uint64 id, BufferSlice packet) override {
    static_assert(
        std::is_same<telegram_api::account_reportPeer::ReturnType, telegram_api::messages_report::ReturnType>::value,
        "");
    auto result_ptr = fetch_result<telegram_api::account_reportPeer>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      return on_error(id, Status::Error(400, "Receive false as result"));
    }

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    LOG(INFO) << "Receive error for report peer: " << status;
    td->messages_manager_->on_get_dialog_error(dialog_id_, status, "ReportPeerQuery");
    promise_.set_error(std::move(status));
  }
};

class GetChannelDifferenceQuery : public Td::ResultHandler {
  DialogId dialog_id_;
  int32 pts_;
  int32 limit_;

 public:
  void send(DialogId dialog_id, tl_object_ptr<telegram_api::InputChannel> &&input_channel, int32 pts, int32 limit,
            bool force) {
    CHECK(pts >= 0);
    dialog_id_ = dialog_id;
    pts_ = pts;
    limit_ = limit;
    CHECK(input_channel != nullptr);

    int32 flags = 0;
    if (force) {
      flags |= telegram_api::updates_getChannelDifference::FORCE_MASK;
    }
    send_query(G()->net_query_creator().create(create_storer(telegram_api::updates_getChannelDifference(
        flags, false /*ignored*/, std::move(input_channel), make_tl_object<telegram_api::channelMessagesFilterEmpty>(),
        pts, limit))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::updates_getChannelDifference>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    td->messages_manager_->on_get_channel_difference(dialog_id_, pts_, limit_, result_ptr.move_as_ok());
  }

  void on_error(uint64 id, Status status) override {
    if (!td->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetChannelDifferenceQuery")) {
      LOG(ERROR) << "updates.getChannelDifference error for " << dialog_id_ << ": " << status;
    }
    td->messages_manager_->on_get_channel_difference(dialog_id_, pts_, limit_, nullptr);
    status.ignore();
  }
};

class ResolveUsernameQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  string username_;

 public:
  explicit ResolveUsernameQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &username) {
    username_ = username;

    LOG(INFO) << "Send ResolveUsernameQuery with username = " << username;
    send_query(G()->net_query_creator().create(create_storer(telegram_api::contacts_resolveUsername(username))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::contacts_resolveUsername>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for resolveUsername " << to_string(ptr);
    td->contacts_manager_->on_get_users(std::move(ptr->users_));
    td->contacts_manager_->on_get_chats(std::move(ptr->chats_));

    td->messages_manager_->on_resolved_username(username_, DialogId(ptr->peer_));

    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    if (status.message() == Slice("USERNAME_NOT_OCCUPIED")) {
      td->messages_manager_->drop_username(username_);
    }
    promise_.set_error(std::move(status));
  }
};

class GetChannelAdminLogQuery : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  int64 random_id_;

 public:
  explicit GetChannelAdminLogQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, const string &query, int64 from_event_id, int32 limit,
            tl_object_ptr<telegram_api::channelAdminLogEventsFilter> filter,
            vector<tl_object_ptr<telegram_api::InputUser>> input_users, int64 random_id) {
    channel_id_ = channel_id;
    random_id_ = random_id;

    auto input_channel = td->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    int32 flags = 0;
    if (filter != nullptr) {
      flags |= telegram_api::channels_getAdminLog::EVENTS_FILTER_MASK;
    }
    if (!input_users.empty()) {
      flags |= telegram_api::channels_getAdminLog::ADMINS_MASK;
    }

    send_query(G()->net_query_creator().create(create_storer(telegram_api::channels_getAdminLog(
        flags, std::move(input_channel), query, std::move(filter), std::move(input_users), from_event_id, 0, limit))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::channels_getAdminLog>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    td->messages_manager_->on_get_event_log(random_id_, result_ptr.move_as_ok());
    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    td->contacts_manager_->on_get_channel_error(channel_id_, status, "GetChannelAdminLogQuery");
    td->messages_manager_->on_get_event_log(random_id_, nullptr);
    promise_.set_error(std::move(status));
  }
};

bool operator==(const InputMessageText &lhs, const InputMessageText &rhs) {
  return lhs.text == rhs.text && lhs.disable_web_page_preview == rhs.disable_web_page_preview &&
         lhs.clear_draft == rhs.clear_draft;
}

bool operator!=(const InputMessageText &lhs, const InputMessageText &rhs) {
  return !(lhs == rhs);
}

class MessagesManager::UploadMediaCallback : public FileManager::UploadCallback {
 public:
  void on_progress(FileId file_id) override {
  }
  void on_upload_ok(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file) override {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_upload_media, file_id, std::move(input_file),
                       nullptr);
  }
  void on_upload_encrypted_ok(FileId file_id, tl_object_ptr<telegram_api::InputEncryptedFile> input_file) override {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_upload_media, file_id, nullptr,
                       std::move(input_file));
  }
  void on_upload_error(FileId file_id, Status error) override {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_upload_media_error, file_id, std::move(error));
  }
};

class MessagesManager::UploadThumbnailCallback : public FileManager::UploadCallback {
 public:
  void on_upload_ok(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file) override {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_upload_thumbnail, file_id, std::move(input_file));
  }
  void on_upload_encrypted_ok(FileId file_id, tl_object_ptr<telegram_api::InputEncryptedFile> input_file) override {
    UNREACHABLE();
  }
  void on_upload_error(FileId file_id, Status error) override {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_upload_thumbnail, file_id, nullptr);
  }
};

class MessagesManager::UploadDialogPhotoCallback : public FileManager::UploadCallback {
 public:
  void on_upload_ok(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file) override {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_upload_dialog_photo, file_id,
                       std::move(input_file));
  }
  void on_upload_encrypted_ok(FileId file_id, tl_object_ptr<telegram_api::InputEncryptedFile> input_file) override {
    UNREACHABLE();
  }
  void on_upload_error(FileId file_id, Status error) override {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_upload_dialog_photo_error, file_id,
                       std::move(error));
  }
};

template <class StorerT>
static void store(const MessageContent *content, StorerT &storer) {
  CHECK(content != nullptr);

  Td *td = storer.context()->td().get_actor_unsafe();
  CHECK(td != nullptr);

  auto content_id = content->get_id();
  store(content_id, storer);

  switch (content_id) {
    case MessageAnimation::ID: {
      auto m = static_cast<const MessageAnimation *>(content);
      td->animations_manager_->store_animation(m->file_id, storer);
      store(m->caption, storer);
      break;
    }
    case MessageAudio::ID: {
      auto m = static_cast<const MessageAudio *>(content);
      td->audios_manager_->store_audio(m->file_id, storer);
      store(m->caption, storer);
      store(true, storer);
      break;
    }
    case MessageContact::ID: {
      auto m = static_cast<const MessageContact *>(content);
      store(m->contact, storer);
      break;
    }
    case MessageDocument::ID: {
      auto m = static_cast<const MessageDocument *>(content);
      td->documents_manager_->store_document(m->file_id, storer);
      store(m->caption, storer);
      break;
    }
    case MessageGame::ID: {
      auto m = static_cast<const MessageGame *>(content);
      store(m->game, storer);
      break;
    }
    case MessageInvoice::ID: {
      auto m = static_cast<const MessageInvoice *>(content);
      store(m->title, storer);
      store(m->description, storer);
      store(m->photo, storer);
      store(m->start_parameter, storer);
      store(m->invoice, storer);
      store(m->payload, storer);
      store(m->provider_token, storer);
      store(m->provider_data, storer);
      store(m->total_amount, storer);
      store(m->receipt_message_id, storer);
      break;
    }
    case MessageLiveLocation::ID: {
      auto m = static_cast<const MessageLiveLocation *>(content);
      store(m->location, storer);
      store(m->period, storer);
      break;
    }
    case MessageLocation::ID: {
      auto m = static_cast<const MessageLocation *>(content);
      store(m->location, storer);
      break;
    }
    case MessagePhoto::ID: {
      auto m = static_cast<const MessagePhoto *>(content);
      store(m->photo, storer);
      store(m->caption, storer);
      break;
    }
    case MessageSticker::ID: {
      auto m = static_cast<const MessageSticker *>(content);
      td->stickers_manager_->store_sticker(m->file_id, false, storer);
      break;
    }
    case MessageText::ID: {
      auto m = static_cast<const MessageText *>(content);
      store(m->text, storer);
      store(m->web_page_id, storer);
      break;
    }
    case MessageUnsupported::ID:
      break;
    case MessageVenue::ID: {
      auto m = static_cast<const MessageVenue *>(content);
      store(m->venue, storer);
      break;
    }
    case MessageVideo::ID: {
      auto m = static_cast<const MessageVideo *>(content);
      td->videos_manager_->store_video(m->file_id, storer);
      store(m->caption, storer);
      break;
    }
    case MessageVideoNote::ID: {
      auto m = static_cast<const MessageVideoNote *>(content);
      td->video_notes_manager_->store_video_note(m->file_id, storer);
      store(m->is_viewed, storer);
      break;
    }
    case MessageVoiceNote::ID: {
      auto m = static_cast<const MessageVoiceNote *>(content);
      td->voice_notes_manager_->store_voice_note(m->file_id, storer);
      store(m->caption, storer);
      store(m->is_listened, storer);
      break;
    }
    case MessageChatCreate::ID: {
      auto m = static_cast<const MessageChatCreate *>(content);
      store(m->title, storer);
      store(m->participant_user_ids, storer);
      break;
    }
    case MessageChatChangeTitle::ID: {
      auto m = static_cast<const MessageChatChangeTitle *>(content);
      store(m->title, storer);
      break;
    }
    case MessageChatChangePhoto::ID: {
      auto m = static_cast<const MessageChatChangePhoto *>(content);
      store(m->photo, storer);
      break;
    }
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
      break;
    case MessageChatAddUsers::ID: {
      auto m = static_cast<const MessageChatAddUsers *>(content);
      store(m->user_ids, storer);
      break;
    }
    case MessageChatJoinedByLink::ID:
      break;
    case MessageChatDeleteUser::ID: {
      auto m = static_cast<const MessageChatDeleteUser *>(content);
      store(m->user_id, storer);
      break;
    }
    case MessageChatMigrateTo::ID: {
      auto m = static_cast<const MessageChatMigrateTo *>(content);
      store(m->migrated_to_channel_id, storer);
      break;
    }
    case MessageChannelCreate::ID: {
      auto m = static_cast<const MessageChannelCreate *>(content);
      store(m->title, storer);
      break;
    }
    case MessageChannelMigrateFrom::ID: {
      auto m = static_cast<const MessageChannelMigrateFrom *>(content);
      store(m->title, storer);
      store(m->migrated_from_chat_id, storer);
      break;
    }
    case MessagePinMessage::ID: {
      auto m = static_cast<const MessagePinMessage *>(content);
      store(m->message_id, storer);
      break;
    }
    case MessageGameScore::ID: {
      auto m = static_cast<const MessageGameScore *>(content);
      store(m->game_message_id, storer);
      store(m->game_id, storer);
      store(m->score, storer);
      break;
    }
    case MessageScreenshotTaken::ID:
      break;
    case MessageChatSetTtl::ID: {
      auto m = static_cast<const MessageChatSetTtl *>(content);
      store(m->ttl, storer);
      break;
    }
    case MessageCall::ID: {
      auto m = static_cast<const MessageCall *>(content);
      store(m->call_id, storer);
      store(m->duration, storer);
      store(m->discard_reason, storer);
      break;
    }
    case MessagePaymentSuccessful::ID: {
      auto m = static_cast<const MessagePaymentSuccessful *>(content);
      bool has_payload = !m->invoice_payload.empty();
      bool has_shipping_option_id = !m->shipping_option_id.empty();
      bool has_order_info = m->order_info != nullptr;
      bool has_telegram_payment_charge_id = !m->telegram_payment_charge_id.empty();
      bool has_provider_payment_charge_id = !m->provider_payment_charge_id.empty();
      bool has_invoice_message_id = m->invoice_message_id.is_valid();
      BEGIN_STORE_FLAGS();
      STORE_FLAG(has_payload);
      STORE_FLAG(has_shipping_option_id);
      STORE_FLAG(has_order_info);
      STORE_FLAG(has_telegram_payment_charge_id);
      STORE_FLAG(has_provider_payment_charge_id);
      STORE_FLAG(has_invoice_message_id);
      END_STORE_FLAGS();
      store(m->currency, storer);
      store(m->total_amount, storer);
      if (has_payload) {
        store(m->total_amount, storer);
      }
      if (has_shipping_option_id) {
        store(m->invoice_payload, storer);
      }
      if (has_order_info) {
        store(*m->order_info, storer);
      }
      if (has_telegram_payment_charge_id) {
        store(m->telegram_payment_charge_id, storer);
      }
      if (has_provider_payment_charge_id) {
        store(m->provider_payment_charge_id, storer);
      }
      if (has_invoice_message_id) {
        store(m->invoice_message_id, storer);
      }
      break;
    }
    case MessageContactRegistered::ID:
      break;
    case MessageExpiredPhoto::ID:
      break;
    case MessageExpiredVideo::ID:
      break;
    case MessageCustomServiceAction::ID: {
      auto m = static_cast<const MessageCustomServiceAction *>(content);
      store(m->message, storer);
      break;
    }
    case MessageWebsiteConnected::ID: {
      auto m = static_cast<const MessageWebsiteConnected *>(content);
      store(m->domain_name, storer);
      break;
    }
    default:
      UNREACHABLE();
  }
}

template <class ParserT>
static void parse_caption(FormattedText &caption, ParserT &parser) {
  parse(caption.text, parser);
  if (parser.version() >= static_cast<int32>(Version::AddCaptionEntities)) {
    parse(caption.entities, parser);
  } else {
    caption.entities = find_entities(caption.text, false);
  }
}

template <class ParserT>
static void parse(unique_ptr<MessageContent> &content, ParserT &parser) {
  Td *td = parser.context()->td().get_actor_unsafe();
  CHECK(td != nullptr);

  int32 content_id;
  parse(content_id, parser);

  bool is_bad = false;
  switch (content_id) {
    case MessageAnimation::ID: {
      auto m = make_unique<MessageAnimation>();
      m->file_id = td->animations_manager_->parse_animation(parser);
      parse_caption(m->caption, parser);
      is_bad = !m->file_id.is_valid();
      content = std::move(m);
      break;
    }
    case MessageAudio::ID: {
      auto m = make_unique<MessageAudio>();
      m->file_id = td->audios_manager_->parse_audio(parser);
      parse_caption(m->caption, parser);
      bool legacy_is_listened;
      parse(legacy_is_listened, parser);
      is_bad = !m->file_id.is_valid();
      content = std::move(m);
      break;
    }
    case MessageContact::ID: {
      auto m = make_unique<MessageContact>();
      parse(m->contact, parser);
      content = std::move(m);
      break;
    }
    case MessageDocument::ID: {
      auto m = make_unique<MessageDocument>();
      m->file_id = td->documents_manager_->parse_document(parser);
      parse_caption(m->caption, parser);
      is_bad = !m->file_id.is_valid();
      content = std::move(m);
      break;
    }
    case MessageGame::ID: {
      auto m = make_unique<MessageGame>();
      parse(m->game, parser);
      content = std::move(m);
      break;
    }
    case MessageInvoice::ID: {
      auto m = make_unique<MessageInvoice>();
      parse(m->title, parser);
      parse(m->description, parser);
      parse(m->photo, parser);
      parse(m->start_parameter, parser);
      parse(m->invoice, parser);
      parse(m->payload, parser);
      parse(m->provider_token, parser);
      if (parser.version() >= static_cast<int32>(Version::AddMessageInvoiceProviderData)) {
        parse(m->provider_data, parser);
      } else {
        m->provider_data.clear();
      }
      parse(m->total_amount, parser);
      parse(m->receipt_message_id, parser);
      content = std::move(m);
      break;
    }
    case MessageLiveLocation::ID: {
      auto m = make_unique<MessageLiveLocation>();
      parse(m->location, parser);
      parse(m->period, parser);
      content = std::move(m);
      break;
    }
    case MessageLocation::ID: {
      auto m = make_unique<MessageLocation>();
      parse(m->location, parser);
      content = std::move(m);
      break;
    }
    case MessagePhoto::ID: {
      auto m = make_unique<MessagePhoto>();
      parse(m->photo, parser);
      for (auto &photo_size : m->photo.photos) {
        if (!photo_size.file_id.is_valid()) {
          is_bad = true;
        }
      }
      parse_caption(m->caption, parser);
      content = std::move(m);
      break;
    }
    case MessageSticker::ID: {
      auto m = make_unique<MessageSticker>();
      m->file_id = td->stickers_manager_->parse_sticker(false, parser);
      is_bad = !m->file_id.is_valid();
      content = std::move(m);
      break;
    }
    case MessageText::ID: {
      auto m = make_unique<MessageText>();
      parse(m->text, parser);
      parse(m->web_page_id, parser);
      content = std::move(m);
      break;
    }
    case MessageUnsupported::ID:
      content = make_unique<MessageUnsupported>();
      break;
    case MessageVenue::ID: {
      auto m = make_unique<MessageVenue>();
      parse(m->venue, parser);
      content = std::move(m);
      break;
    }
    case MessageVideo::ID: {
      auto m = make_unique<MessageVideo>();
      m->file_id = td->videos_manager_->parse_video(parser);
      parse_caption(m->caption, parser);
      is_bad = !m->file_id.is_valid();
      content = std::move(m);
      break;
    }
    case MessageVideoNote::ID: {
      auto m = make_unique<MessageVideoNote>();
      m->file_id = td->video_notes_manager_->parse_video_note(parser);
      parse(m->is_viewed, parser);
      is_bad = !m->file_id.is_valid();
      content = std::move(m);
      break;
    }
    case MessageVoiceNote::ID: {
      auto m = make_unique<MessageVoiceNote>();
      m->file_id = td->voice_notes_manager_->parse_voice_note(parser);
      parse_caption(m->caption, parser);
      parse(m->is_listened, parser);
      is_bad = !m->file_id.is_valid();
      content = std::move(m);
      break;
    }
    case MessageChatCreate::ID: {
      auto m = make_unique<MessageChatCreate>();
      parse(m->title, parser);
      parse(m->participant_user_ids, parser);
      content = std::move(m);
      break;
    }
    case MessageChatChangeTitle::ID: {
      auto m = make_unique<MessageChatChangeTitle>();
      parse(m->title, parser);
      content = std::move(m);
      break;
    }
    case MessageChatChangePhoto::ID: {
      auto m = make_unique<MessageChatChangePhoto>();
      parse(m->photo, parser);
      content = std::move(m);
      break;
    }
    case MessageChatDeletePhoto::ID:
      content = make_unique<MessageChatDeletePhoto>();
      break;
    case MessageChatDeleteHistory::ID:
      content = make_unique<MessageChatDeleteHistory>();
      break;
    case MessageChatAddUsers::ID: {
      auto m = make_unique<MessageChatAddUsers>();
      parse(m->user_ids, parser);
      content = std::move(m);
      break;
    }
    case MessageChatJoinedByLink::ID:
      content = make_unique<MessageChatJoinedByLink>();
      break;
    case MessageChatDeleteUser::ID: {
      auto m = make_unique<MessageChatDeleteUser>();
      parse(m->user_id, parser);
      content = std::move(m);
      break;
    }
    case MessageChatMigrateTo::ID: {
      auto m = make_unique<MessageChatMigrateTo>();
      parse(m->migrated_to_channel_id, parser);
      content = std::move(m);
      break;
    }
    case MessageChannelCreate::ID: {
      auto m = make_unique<MessageChannelCreate>();
      parse(m->title, parser);
      content = std::move(m);
      break;
    }
    case MessageChannelMigrateFrom::ID: {
      auto m = make_unique<MessageChannelMigrateFrom>();
      parse(m->title, parser);
      parse(m->migrated_from_chat_id, parser);
      content = std::move(m);
      break;
    }
    case MessagePinMessage::ID: {
      auto m = make_unique<MessagePinMessage>();
      parse(m->message_id, parser);
      content = std::move(m);
      break;
    }
    case MessageGameScore::ID: {
      auto m = make_unique<MessageGameScore>();
      parse(m->game_message_id, parser);
      parse(m->game_id, parser);
      parse(m->score, parser);
      content = std::move(m);
      break;
    }
    case MessageScreenshotTaken::ID:
      content = make_unique<MessageScreenshotTaken>();
      break;
    case MessageChatSetTtl::ID: {
      auto m = make_unique<MessageChatSetTtl>();
      parse(m->ttl, parser);
      content = std::move(m);
      break;
    }
    case MessageCall::ID: {
      auto m = make_unique<MessageCall>();
      parse(m->call_id, parser);
      parse(m->duration, parser);
      parse(m->discard_reason, parser);
      content = std::move(m);
      break;
    }
    case MessagePaymentSuccessful::ID: {
      auto m = make_unique<MessagePaymentSuccessful>();
      bool has_payload;
      bool has_shipping_option_id;
      bool has_order_info;
      bool has_telegram_payment_charge_id;
      bool has_provider_payment_charge_id;
      bool has_invoice_message_id;
      BEGIN_PARSE_FLAGS();
      PARSE_FLAG(has_payload);
      PARSE_FLAG(has_shipping_option_id);
      PARSE_FLAG(has_order_info);
      PARSE_FLAG(has_telegram_payment_charge_id);
      PARSE_FLAG(has_provider_payment_charge_id);
      PARSE_FLAG(has_invoice_message_id);
      END_PARSE_FLAGS();
      parse(m->currency, parser);
      parse(m->total_amount, parser);
      if (has_payload) {
        parse(m->total_amount, parser);
      }
      if (has_shipping_option_id) {
        parse(m->invoice_payload, parser);
      }
      if (has_order_info) {
        m->order_info = make_unique<OrderInfo>();
        parse(*m->order_info, parser);
      }
      if (has_telegram_payment_charge_id) {
        parse(m->telegram_payment_charge_id, parser);
      }
      if (has_provider_payment_charge_id) {
        parse(m->provider_payment_charge_id, parser);
      }
      if (has_invoice_message_id) {
        parse(m->invoice_message_id, parser);
      }
      content = std::move(m);
      break;
    }
    case MessageContactRegistered::ID:
      content = make_unique<MessageContactRegistered>();
      break;
    case MessageExpiredPhoto::ID:
      content = make_unique<MessageExpiredPhoto>();
      break;
    case MessageExpiredVideo::ID:
      content = make_unique<MessageExpiredVideo>();
      break;
    case MessageCustomServiceAction::ID: {
      auto m = make_unique<MessageCustomServiceAction>();
      parse(m->message, parser);
      content = std::move(m);
      break;
    }
    case MessageWebsiteConnected::ID: {
      auto m = make_unique<MessageWebsiteConnected>();
      parse(m->domain_name, parser);
      content = std::move(m);
      break;
    }
    default:
      UNREACHABLE();
  }
  if (is_bad) {
    LOG(ERROR) << "Load a message with an invalid content of type " << content_id;
    content = make_unique<MessageUnsupported>();
  }
}

template <class StorerT>
void MessagesManager::Message::store(StorerT &storer) const {
  using td::store;
  bool has_sender = sender_user_id.is_valid();
  bool has_edit_date = edit_date > 0;
  bool has_random_id = random_id != 0;
  bool is_forwarded = forward_info != nullptr;
  bool is_reply = reply_to_message_id.is_valid();
  bool is_reply_to_random_id = reply_to_random_id != 0;
  bool is_via_bot = via_bot_user_id.is_valid();
  bool has_views = views > 0;
  bool has_reply_markup = reply_markup != nullptr;
  bool has_ttl = ttl != 0;
  bool has_author_signature = !author_signature.empty();
  bool has_forward_author_signature = is_forwarded && !forward_info->author_signature.empty();
  bool has_media_album_id = media_album_id != 0;
  bool has_forward_from =
      is_forwarded && (forward_info->from_dialog_id.is_valid() || forward_info->from_message_id.is_valid());
  BEGIN_STORE_FLAGS();
  STORE_FLAG(is_channel_post);
  STORE_FLAG(is_outgoing);
  STORE_FLAG(is_failed_to_send);
  STORE_FLAG(disable_notification);
  STORE_FLAG(contains_mention);
  STORE_FLAG(from_background);
  STORE_FLAG(disable_web_page_preview);
  STORE_FLAG(clear_draft);
  STORE_FLAG(have_previous);
  STORE_FLAG(have_next);
  STORE_FLAG(has_sender);
  STORE_FLAG(has_edit_date);
  STORE_FLAG(has_random_id);
  STORE_FLAG(is_forwarded);
  STORE_FLAG(is_reply);
  STORE_FLAG(is_reply_to_random_id);
  STORE_FLAG(is_via_bot);
  STORE_FLAG(has_views);
  STORE_FLAG(has_reply_markup);
  STORE_FLAG(has_ttl);
  STORE_FLAG(has_author_signature);
  STORE_FLAG(has_forward_author_signature);
  STORE_FLAG(had_reply_markup);
  STORE_FLAG(contains_unread_mention);
  STORE_FLAG(has_media_album_id);
  STORE_FLAG(has_forward_from);
  STORE_FLAG(in_game_share);
  STORE_FLAG(is_content_secret);
  END_STORE_FLAGS();

  store(message_id, storer);
  if (has_sender) {
    store(sender_user_id, storer);
  }
  store(date, storer);
  if (has_edit_date) {
    store(edit_date, storer);
  }
  if (has_random_id) {
    store(random_id, storer);
  }
  if (is_forwarded) {
    store(forward_info->sender_user_id, storer);
    store(forward_info->date, storer);
    store(forward_info->dialog_id, storer);
    store(forward_info->message_id, storer);
    if (has_forward_author_signature) {
      store(forward_info->author_signature, storer);
    }
    if (has_forward_from) {
      store(forward_info->from_dialog_id, storer);
      store(forward_info->from_message_id, storer);
    }
  }
  if (is_reply) {
    store(reply_to_message_id, storer);
  }
  if (is_reply_to_random_id) {
    store(reply_to_random_id, storer);
  }
  if (is_via_bot) {
    store(via_bot_user_id, storer);
  }
  if (has_views) {
    store(views, storer);
  }
  if (has_ttl) {
    store(ttl, storer);
    double server_time = storer.context()->server_time();
    if (ttl_expires_at == 0) {
      store(-1.0, storer);
    } else {
      double ttl_left = max(ttl_expires_at - Time::now_cached(), 0.0);
      store(ttl_left, storer);
      store(server_time, storer);
    }
  }
  if (has_author_signature) {
    store(author_signature, storer);
  }
  if (has_media_album_id) {
    store(media_album_id, storer);
  }
  store(static_cast<const MessageContent *>(content.get()), storer);  // TODO unique_ptr with const propagation
  if (has_reply_markup) {
    store(*reply_markup, storer);
  }
}

// do not forget to resolve message dependencies
template <class ParserT>
void MessagesManager::Message::parse(ParserT &parser) {
  using td::parse;
  bool has_sender;
  bool has_edit_date;
  bool has_random_id;
  bool is_forwarded;
  bool is_reply;
  bool is_reply_to_random_id;
  bool is_via_bot;
  bool has_views;
  bool has_reply_markup;
  bool has_ttl;
  bool has_author_signature;
  bool has_forward_author_signature;
  bool has_media_album_id;
  bool has_forward_from;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(is_channel_post);
  PARSE_FLAG(is_outgoing);
  PARSE_FLAG(is_failed_to_send);
  PARSE_FLAG(disable_notification);
  PARSE_FLAG(contains_mention);
  PARSE_FLAG(from_background);
  PARSE_FLAG(disable_web_page_preview);
  PARSE_FLAG(clear_draft);
  PARSE_FLAG(have_previous);
  PARSE_FLAG(have_next);
  PARSE_FLAG(has_sender);
  PARSE_FLAG(has_edit_date);
  PARSE_FLAG(has_random_id);
  PARSE_FLAG(is_forwarded);
  PARSE_FLAG(is_reply);
  PARSE_FLAG(is_reply_to_random_id);
  PARSE_FLAG(is_via_bot);
  PARSE_FLAG(has_views);
  PARSE_FLAG(has_reply_markup);
  PARSE_FLAG(has_ttl);
  PARSE_FLAG(has_author_signature);
  PARSE_FLAG(has_forward_author_signature);
  PARSE_FLAG(had_reply_markup);
  PARSE_FLAG(contains_unread_mention);
  PARSE_FLAG(has_media_album_id);
  PARSE_FLAG(has_forward_from);
  PARSE_FLAG(in_game_share);
  PARSE_FLAG(is_content_secret);
  END_PARSE_FLAGS();

  parse(message_id, parser);
  random_y = get_random_y(message_id);
  if (has_sender) {
    parse(sender_user_id, parser);
  }
  parse(date, parser);
  if (has_edit_date) {
    parse(edit_date, parser);
  }
  if (has_random_id) {
    parse(random_id, parser);
  }
  if (is_forwarded) {
    forward_info = make_unique<MessageForwardInfo>();
    parse(forward_info->sender_user_id, parser);
    parse(forward_info->date, parser);
    parse(forward_info->dialog_id, parser);
    parse(forward_info->message_id, parser);
    if (has_forward_author_signature) {
      parse(forward_info->author_signature, parser);
    }
    if (has_forward_from) {
      parse(forward_info->from_dialog_id, parser);
      parse(forward_info->from_message_id, parser);
    }
  }
  if (is_reply) {
    parse(reply_to_message_id, parser);
  }
  if (is_reply_to_random_id) {
    parse(reply_to_random_id, parser);
  }
  if (is_via_bot) {
    parse(via_bot_user_id, parser);
  }
  if (has_views) {
    parse(views, parser);
  }
  if (has_ttl) {
    parse(ttl, parser);
    double ttl_left;
    parse(ttl_left, parser);
    if (ttl_left < -0.1) {
      ttl_expires_at = 0;
    } else {
      double old_server_time;
      parse(old_server_time, parser);
      double passed_server_time = max(parser.context()->server_time() - old_server_time, 0.0);
      ttl_left = max(ttl_left - passed_server_time, 0.0);
      ttl_expires_at = Time::now_cached() + ttl_left;
    }
  }
  if (has_author_signature) {
    parse(author_signature, parser);
  }
  if (has_media_album_id) {
    parse(media_album_id, parser);
  }
  parse(content, parser);
  if (has_reply_markup) {
    reply_markup = make_unique<ReplyMarkup>();
    parse(*reply_markup, parser);
  }
  is_content_secret |= is_secret_message_content(ttl, content->get_id());  // repair is_content_secret for old messages
}

template <class StorerT>
void store(const NotificationSettings &notification_settings, StorerT &storer) {
  bool is_muted = notification_settings.mute_until != 0 && notification_settings.mute_until > G()->unix_time();
  bool has_sound = notification_settings.sound != "default";
  BEGIN_STORE_FLAGS();
  STORE_FLAG(is_muted);
  STORE_FLAG(has_sound);
  STORE_FLAG(notification_settings.show_preview);
  STORE_FLAG(notification_settings.silent_send_message);
  STORE_FLAG(notification_settings.is_synchronized);
  END_STORE_FLAGS();
  if (is_muted) {
    store(notification_settings.mute_until, storer);
  }
  if (has_sound) {
    store(notification_settings.sound, storer);
  }
}

template <class ParserT>
void parse(NotificationSettings &notification_settings, ParserT &parser) {
  bool is_muted;
  bool has_sound;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(is_muted);
  PARSE_FLAG(has_sound);
  PARSE_FLAG(notification_settings.show_preview);
  PARSE_FLAG(notification_settings.silent_send_message);
  PARSE_FLAG(notification_settings.is_synchronized);
  END_PARSE_FLAGS();
  if (is_muted) {
    parse(notification_settings.mute_until, parser);
  }
  if (has_sound) {
    parse(notification_settings.sound, parser);
  }
}

template <class StorerT>
void store(const InputMessageText &input_message_text, StorerT &storer) {
  BEGIN_STORE_FLAGS();
  STORE_FLAG(input_message_text.disable_web_page_preview);
  STORE_FLAG(input_message_text.clear_draft);
  END_STORE_FLAGS();
  store(input_message_text.text, storer);
}

template <class ParserT>
void parse(InputMessageText &input_message_text, ParserT &parser) {
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(input_message_text.disable_web_page_preview);
  PARSE_FLAG(input_message_text.clear_draft);
  END_PARSE_FLAGS();
  parse(input_message_text.text, parser);
}

template <class StorerT>
void store(const DraftMessage &draft_message, StorerT &storer) {
  store(draft_message.date, storer);
  store(draft_message.reply_to_message_id, storer);
  store(draft_message.input_message_text, storer);
}

template <class ParserT>
void parse(DraftMessage &draft_message, ParserT &parser) {
  parse(draft_message.date, parser);
  parse(draft_message.reply_to_message_id, parser);
  parse(draft_message.input_message_text, parser);
}

template <class StorerT>
void MessagesManager::Dialog::store(StorerT &storer) const {
  using td::store;
  const Message *last_database_message = nullptr;
  if (last_database_message_id.is_valid()) {
    last_database_message = get_message(this, last_database_message_id);
  }

  bool has_draft_message = draft_message != nullptr;
  bool has_last_database_message = last_database_message != nullptr;
  bool has_first_database_message_id = first_database_message_id.is_valid();
  bool is_pinned = pinned_order != DEFAULT_ORDER;
  bool has_first_database_message_id_by_index = true;
  bool has_message_count_by_index = true;
  bool has_client_data = !client_data.empty();
  bool has_last_read_all_mentions_message_id = last_read_all_mentions_message_id.is_valid();
  bool has_max_unavailable_message_id = max_unavailable_message_id.is_valid();
  bool has_local_unread_count = local_unread_count != 0;
  bool has_deleted_last_message = delete_last_message_date > 0;
  bool has_last_clear_history_message_id = last_clear_history_message_id.is_valid();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(has_draft_message);
  STORE_FLAG(has_last_database_message);
  STORE_FLAG(know_can_report_spam);
  STORE_FLAG(can_report_spam);
  STORE_FLAG(has_first_database_message_id);
  STORE_FLAG(is_pinned);
  STORE_FLAG(has_first_database_message_id_by_index);
  STORE_FLAG(has_message_count_by_index);
  STORE_FLAG(has_client_data);
  STORE_FLAG(need_restore_reply_markup);
  STORE_FLAG(have_full_history);
  STORE_FLAG(has_last_read_all_mentions_message_id);
  STORE_FLAG(has_max_unavailable_message_id);
  STORE_FLAG(is_last_read_inbox_message_id_inited);
  STORE_FLAG(is_last_read_outbox_message_id_inited);
  STORE_FLAG(has_local_unread_count);
  STORE_FLAG(has_deleted_last_message);
  STORE_FLAG(has_last_clear_history_message_id);
  STORE_FLAG(is_last_message_deleted_locally);
  STORE_FLAG(has_contact_registered_message);
  END_STORE_FLAGS();

  store(dialog_id, storer);  // must be stored at offset 4
  store(last_new_message_id, storer);
  store(server_unread_count, storer);
  if (has_local_unread_count) {
    store(local_unread_count, storer);
  }
  store(last_read_inbox_message_id, storer);
  store(last_read_outbox_message_id, storer);
  store(reply_markup_message_id, storer);
  store(notification_settings, storer);
  if (has_draft_message) {
    store(*draft_message, storer);
  }
  store(last_clear_history_date, storer);
  store(order, storer);
  if (has_last_database_message) {
    store(*last_database_message, storer);
  }
  if (has_first_database_message_id) {
    store(first_database_message_id, storer);
  }
  if (is_pinned) {
    store(pinned_order, storer);
  }
  if (has_deleted_last_message) {
    store(delete_last_message_date, storer);
    store(deleted_last_message_id, storer);
  }
  if (has_last_clear_history_message_id) {
    store(last_clear_history_message_id, storer);
  }

  if (has_first_database_message_id_by_index) {
    store(static_cast<int32>(first_database_message_id_by_index.size()), storer);
    for (auto first_message_id : first_database_message_id_by_index) {
      store(first_message_id, storer);
    }
  }
  if (has_message_count_by_index) {
    store(static_cast<int32>(message_count_by_index.size()), storer);
    for (auto message_count : message_count_by_index) {
      store(message_count, storer);
    }
  }
  if (has_client_data) {
    store(client_data, storer);
  }
  if (has_last_read_all_mentions_message_id) {
    store(last_read_all_mentions_message_id, storer);
  }
  if (has_max_unavailable_message_id) {
    store(max_unavailable_message_id, storer);
  }
}

// do not forget to resolve dialog dependencies including dependencies of last_message
template <class ParserT>
void MessagesManager::Dialog::parse(ParserT &parser) {
  using td::parse;
  bool has_draft_message;
  bool has_last_database_message;
  bool has_first_database_message_id;
  bool is_pinned;
  bool has_first_database_message_id_by_index;
  bool has_message_count_by_index;
  bool has_client_data;
  bool has_last_read_all_mentions_message_id;
  bool has_max_unavailable_message_id;
  bool has_local_unread_count;
  bool has_deleted_last_message;
  bool has_last_clear_history_message_id;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(has_draft_message);
  PARSE_FLAG(has_last_database_message);
  PARSE_FLAG(know_can_report_spam);
  PARSE_FLAG(can_report_spam);
  PARSE_FLAG(has_first_database_message_id);
  PARSE_FLAG(is_pinned);
  PARSE_FLAG(has_first_database_message_id_by_index);
  PARSE_FLAG(has_message_count_by_index);
  PARSE_FLAG(has_client_data);
  PARSE_FLAG(need_restore_reply_markup);
  PARSE_FLAG(have_full_history);
  PARSE_FLAG(has_last_read_all_mentions_message_id);
  PARSE_FLAG(has_max_unavailable_message_id);
  PARSE_FLAG(is_last_read_inbox_message_id_inited);
  PARSE_FLAG(is_last_read_outbox_message_id_inited);
  PARSE_FLAG(has_local_unread_count);
  PARSE_FLAG(has_deleted_last_message);
  PARSE_FLAG(has_last_clear_history_message_id);
  PARSE_FLAG(is_last_message_deleted_locally);
  PARSE_FLAG(has_contact_registered_message);
  END_PARSE_FLAGS();

  parse(dialog_id, parser);  // must be stored at offset 4
  parse(last_new_message_id, parser);
  parse(server_unread_count, parser);
  if (has_local_unread_count) {
    parse(local_unread_count, parser);
  }
  parse(last_read_inbox_message_id, parser);
  if (last_read_inbox_message_id.is_valid()) {
    is_last_read_inbox_message_id_inited = true;
  }
  parse(last_read_outbox_message_id, parser);
  if (last_read_outbox_message_id.is_valid()) {
    is_last_read_outbox_message_id_inited = true;
  }
  parse(reply_markup_message_id, parser);
  parse(notification_settings, parser);
  if (has_draft_message) {
    draft_message = make_unique<DraftMessage>();
    parse(*draft_message, parser);
  }
  parse(last_clear_history_date, parser);
  parse(order, parser);
  if (has_last_database_message) {
    messages = make_unique<Message>();
    parse(*messages, parser);
  }
  if (has_first_database_message_id) {
    parse(first_database_message_id, parser);
  }
  if (is_pinned) {
    parse(pinned_order, parser);
  }
  if (has_deleted_last_message) {
    parse(delete_last_message_date, parser);
    parse(deleted_last_message_id, parser);
  }
  if (has_last_clear_history_message_id) {
    parse(last_clear_history_message_id, parser);
  }

  if (has_first_database_message_id_by_index) {
    int32 size;
    parse(size, parser);
    CHECK(static_cast<size_t>(size) <= first_database_message_id_by_index.size())
        << size << " " << first_database_message_id_by_index.size();
    for (int32 i = 0; i < size; i++) {
      parse(first_database_message_id_by_index[i], parser);
    }
  }
  if (has_message_count_by_index) {
    int32 size;
    parse(size, parser);
    CHECK(static_cast<size_t>(size) <= message_count_by_index.size());
    for (int32 i = 0; i < size; i++) {
      parse(message_count_by_index[i], parser);
    }
  }
  unread_mention_count = message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)];
  LOG(INFO) << "Set unread mention message count in " << dialog_id << " to " << unread_mention_count;
  if (unread_mention_count < 0) {
    unread_mention_count = 0;
  }
  if (has_client_data) {
    parse(client_data, parser);
  }
  if (has_last_read_all_mentions_message_id) {
    parse(last_read_all_mentions_message_id, parser);
  }
  if (has_max_unavailable_message_id) {
    parse(max_unavailable_message_id, parser);
  }
}

template <class StorerT>
void store(const CallsDbState &state, StorerT &storer) {
  store(static_cast<int32>(state.first_calls_database_message_id_by_index.size()), storer);
  for (auto first_message_id : state.first_calls_database_message_id_by_index) {
    store(first_message_id, storer);
  }
  store(static_cast<int32>(state.message_count_by_index.size()), storer);
  for (auto message_count : state.message_count_by_index) {
    store(message_count, storer);
  }
}

template <class ParserT>
void parse(CallsDbState &state, ParserT &parser) {
  int32 size;
  parse(size, parser);
  CHECK(static_cast<size_t>(size) <= state.first_calls_database_message_id_by_index.size())
      << size << " " << state.first_calls_database_message_id_by_index.size();
  for (int32 i = 0; i < size; i++) {
    parse(state.first_calls_database_message_id_by_index[i], parser);
  }
  parse(size, parser);
  CHECK(static_cast<size_t>(size) <= state.message_count_by_index.size());
  for (int32 i = 0; i < size; i++) {
    parse(state.message_count_by_index[i], parser);
  }
}

void MessagesManager::load_calls_db_state() {
  if (!G()->parameters().use_message_db) {
    return;
  }
  std::fill(calls_db_state_.message_count_by_index.begin(), calls_db_state_.message_count_by_index.end(), -1);
  auto value = G()->td_db()->get_sqlite_sync_pmc()->get("calls_db_state");
  if (value.empty()) {
    return;
  }
  log_event_parse(calls_db_state_, value).ensure();
  LOG(INFO) << "Save calls database state " << calls_db_state_.first_calls_database_message_id_by_index[0] << " ("
            << calls_db_state_.message_count_by_index[0] << ") "
            << calls_db_state_.first_calls_database_message_id_by_index[1] << " ("
            << calls_db_state_.message_count_by_index[1] << ")";
}

void MessagesManager::save_calls_db_state() {
  if (!G()->parameters().use_message_db) {
    return;
  }

  LOG(INFO) << "Save calls database state " << calls_db_state_.first_calls_database_message_id_by_index[0] << " ("
            << calls_db_state_.message_count_by_index[0] << ") "
            << calls_db_state_.first_calls_database_message_id_by_index[1] << " ("
            << calls_db_state_.message_count_by_index[1] << ")";
  G()->td_db()->get_sqlite_pmc()->set("calls_db_state", log_event_store(calls_db_state_).as_slice().str(), Auto());
}

MessagesManager::Dialog::~Dialog() {
  if (!G()->close_flag()) {
    LOG(ERROR) << "Destroy " << dialog_id;
  }
}

MessagesManager::MessagesManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
  //  td_->create_handler<GetDialogListQuery>()->send(2147000000, ServerMessageId(), DialogId(), 5);

  upload_media_callback_ = std::make_shared<UploadMediaCallback>();
  upload_thumbnail_callback_ = std::make_shared<UploadThumbnailCallback>();
  upload_dialog_photo_callback_ = std::make_shared<UploadDialogPhotoCallback>();

  channel_get_difference_timeout_.set_callback(on_channel_get_difference_timeout_callback);
  channel_get_difference_timeout_.set_callback_data(static_cast<void *>(this));

  channel_get_difference_retry_timeout_.set_callback(on_channel_get_difference_timeout_callback);
  channel_get_difference_retry_timeout_.set_callback_data(static_cast<void *>(this));

  pending_message_views_timeout_.set_callback(on_pending_message_views_timeout_callback);
  pending_message_views_timeout_.set_callback_data(static_cast<void *>(this));

  pending_draft_message_timeout_.set_callback(on_pending_draft_message_timeout_callback);
  pending_draft_message_timeout_.set_callback_data(static_cast<void *>(this));

  pending_updated_dialog_timeout_.set_callback(on_pending_updated_dialog_timeout_callback);
  pending_updated_dialog_timeout_.set_callback_data(static_cast<void *>(this));

  pending_unload_dialog_timeout_.set_callback(on_pending_unload_dialog_timeout_callback);
  pending_unload_dialog_timeout_.set_callback_data(static_cast<void *>(this));

  dialog_unmute_timeout_.set_callback(on_dialog_unmute_timeout_callback);
  dialog_unmute_timeout_.set_callback_data(static_cast<void *>(this));

  pending_send_dialog_action_timeout_.set_callback(on_pending_send_dialog_action_timeout_callback);
  pending_send_dialog_action_timeout_.set_callback_data(static_cast<void *>(this));

  active_dialog_action_timeout_.set_callback(on_active_dialog_action_timeout_callback);
  active_dialog_action_timeout_.set_callback_data(static_cast<void *>(this));

  sequence_dispatcher_ = create_actor<MultiSequenceDispatcher>("multi sequence dispatcher");

  if (G()->parameters().use_message_db) {
    auto last_database_server_dialog_date_string = G()->td_db()->get_binlog_pmc()->get("last_server_dialog_date");
    if (!last_database_server_dialog_date_string.empty()) {
      string order_str;
      string dialog_id_str;
      std::tie(order_str, dialog_id_str) = split(last_database_server_dialog_date_string);

      auto r_order = to_integer_safe<int64>(order_str);
      auto r_dialog_id = to_integer_safe<int64>(dialog_id_str);
      if (r_order.is_error() || r_dialog_id.is_error()) {
        LOG(ERROR) << "Can't parse " << last_database_server_dialog_date_string;
      } else {
        last_database_server_dialog_date_ = DialogDate(r_order.ok(), DialogId(r_dialog_id.ok()));
      }
    }
    LOG(INFO) << "Load last_database_server_dialog_date_ = " << last_database_server_dialog_date_;

    auto unread_message_count_string = G()->td_db()->get_binlog_pmc()->get("unread_message_count");
    if (!unread_message_count_string.empty()) {
      string total_count;
      string muted_count;
      std::tie(total_count, muted_count) = split(unread_message_count_string);

      auto r_total_count = to_integer_safe<int32>(total_count);
      auto r_muted_count = to_integer_safe<int32>(muted_count);
      if (r_total_count.is_error() || r_muted_count.is_error()) {
        LOG(ERROR) << "Can't parse " << unread_message_count_string;
      } else {
        unread_message_total_count_ = r_total_count.ok();
        unread_message_muted_count_ = r_muted_count.ok();
        is_unread_count_inited_ = true;
        send_update_unread_message_count(DialogId(), true, "load unread_message_count");
      }
    }
    LOG(INFO) << "Load last_database_server_dialog_date_ = " << last_database_server_dialog_date_;
  } else {
    G()->td_db()->get_binlog_pmc()->erase("last_server_dialog_date");
    G()->td_db()->get_binlog_pmc()->erase("unread_message_count");
  }
}

MessagesManager::~MessagesManager() = default;

void MessagesManager::on_channel_get_difference_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  DialogId dialog_id(dialog_id_int);
  CHECK(dialog_id.get_type() == DialogType::Channel);
  auto d = messages_manager->get_dialog(dialog_id);
  CHECK(d != nullptr);
  messages_manager->get_channel_difference(dialog_id, d->pts, true, "on_channel_get_difference_timeout_callback");
}

void MessagesManager::on_pending_message_views_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  DialogId dialog_id(dialog_id_int);
  auto d = messages_manager->get_dialog(dialog_id);
  CHECK(d != nullptr);
  CHECK(!d->pending_viewed_message_ids.empty());

  const size_t MAX_MESSAGE_VIEWS = 100;  // server side limit
  vector<MessageId> message_ids;
  message_ids.reserve(min(d->pending_viewed_message_ids.size(), MAX_MESSAGE_VIEWS));
  for (auto message_id : d->pending_viewed_message_ids) {
    message_ids.push_back(message_id);
    if (message_ids.size() >= MAX_MESSAGE_VIEWS) {
      messages_manager->td_->create_handler<GetMessagesViewsQuery>()->send(dialog_id, std::move(message_ids),
                                                                           d->increment_view_counter);
      message_ids.clear();
    }
  }
  if (!message_ids.empty()) {
    messages_manager->td_->create_handler<GetMessagesViewsQuery>()->send(dialog_id, std::move(message_ids),
                                                                         d->increment_view_counter);
  }
  d->pending_viewed_message_ids.clear();
  d->increment_view_counter = false;
}

void MessagesManager::on_pending_draft_message_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  DialogId dialog_id(dialog_id_int);
  messages_manager->save_dialog_draft_message_on_server(dialog_id);
}

void MessagesManager::on_pending_updated_dialog_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  // TODO it is unsafe to save dialog to database before binlog is flushed
  messages_manager->save_dialog_to_database(DialogId(dialog_id_int));
}

void MessagesManager::on_pending_unload_dialog_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  messages_manager->unload_dialog(DialogId(dialog_id_int));
}

void MessagesManager::on_dialog_unmute_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  send_closure_later(messages_manager->actor_id(messages_manager), &MessagesManager::on_dialog_unmute,
                     DialogId(dialog_id_int));
}

void MessagesManager::on_pending_send_dialog_action_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  send_closure_later(messages_manager->actor_id(messages_manager), &MessagesManager::on_send_dialog_action_timeout,
                     DialogId(dialog_id_int));
}

void MessagesManager::on_active_dialog_action_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto messages_manager = static_cast<MessagesManager *>(messages_manager_ptr);
  send_closure_later(messages_manager->actor_id(messages_manager), &MessagesManager::on_active_dialog_action_timeout,
                     DialogId(dialog_id_int));
}

BufferSlice MessagesManager::get_dialog_database_value(const Dialog *d) {
  // can't use log_event_store, because it tries to parse stored Dialog
  LogEventStorerCalcLength storer_calc_length;
  store(*d, storer_calc_length);

  BufferSlice value_buffer{storer_calc_length.get_length()};
  auto value = value_buffer.as_slice();

  LogEventStorerUnsafe storer_unsafe(value.begin());
  store(*d, storer_unsafe);
  return value_buffer;
}

void MessagesManager::save_dialog_to_database(DialogId dialog_id) {
  CHECK(G()->parameters().use_message_db);
  auto d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  LOG(INFO) << "Save " << dialog_id << " to database";
  G()->td_db()->get_dialog_db_async()->add_dialog(
      dialog_id, d->order, get_dialog_database_value(d), PromiseCreator::lambda([dialog_id](Result<> result) {
        send_closure(G()->messages_manager(), &MessagesManager::on_save_dialog_to_database, dialog_id, result.is_ok());
      }));
}

void MessagesManager::on_save_dialog_to_database(DialogId dialog_id, bool success) {
  LOG(INFO) << "Successfully saved " << dialog_id << " to database";
  // TODO erase some events from binlog
}

void MessagesManager::update_message_count_by_index(Dialog *d, int diff, const Message *m) {
  auto index_mask = get_message_index_mask(d->dialog_id, m);
  index_mask &= ~search_messages_filter_index_mask(
      SearchMessagesFilter::UnreadMention);  // unread mention count has been already manually updated
  if (index_mask == 0) {
    return;
  }

  int i = 0;
  for (auto &message_count : d->message_count_by_index) {
    if (((index_mask >> i) & 1) != 0 && message_count != -1) {
      message_count += diff;
      if (message_count < 0) {
        if (d->dialog_id.get_type() == DialogType::SecretChat) {
          message_count = 0;
        } else {
          message_count = -1;
        }
      }
      on_dialog_updated(d->dialog_id, "update_message_count_by_index");
    }
    i++;
  }

  i = static_cast<int>(SearchMessagesFilter::Call) - 1;
  for (auto &message_count : calls_db_state_.message_count_by_index) {
    if (((index_mask >> i) & 1) != 0 && message_count != -1) {
      message_count += diff;
      if (message_count < 0) {
        if (d->dialog_id.get_type() == DialogType::SecretChat) {
          message_count = 0;
        } else {
          message_count = -1;
        }
      }
      save_calls_db_state();
    }
    i++;
  }
}

int32 MessagesManager::get_message_index_mask(DialogId dialog_id, const Message *m) const {
  CHECK(m != nullptr);
  if (m->message_id.is_yet_unsent() || m->is_failed_to_send) {
    return 0;
  }
  bool is_secret = dialog_id.get_type() == DialogType::SecretChat;
  if (!m->message_id.is_server() && !is_secret) {
    return 0;
  }
  // retain second condition just in case
  if (m->is_content_secret || (m->ttl > 0 && !is_secret)) {
    return 0;
  }
  int32 mentions_mask = get_message_content_index_mask(m->content.get(), is_secret, m->is_outgoing);
  if (m->contains_mention) {
    mentions_mask |= search_messages_filter_index_mask(SearchMessagesFilter::Mention);
    if (m->contains_unread_mention) {
      mentions_mask |= search_messages_filter_index_mask(SearchMessagesFilter::UnreadMention);
    }
  }
  return mentions_mask;
}

int32 MessagesManager::get_message_content_index_mask(const MessageContent *content, bool is_secret,
                                                      bool is_outgoing) const {
  switch (content->get_id()) {
    case MessageAnimation::ID:
      return search_messages_filter_index_mask(SearchMessagesFilter::Animation);
    case MessageAudio::ID: {
      auto message_audio = static_cast<const MessageAudio *>(content);
      auto duration = td_->audios_manager_->get_audio_duration(message_audio->file_id);
      return is_secret || duration > 0 ? search_messages_filter_index_mask(SearchMessagesFilter::Audio)
                                       : search_messages_filter_index_mask(SearchMessagesFilter::Document);
    }
    case MessageDocument::ID:
      return search_messages_filter_index_mask(SearchMessagesFilter::Document);
    case MessagePhoto::ID:
      return search_messages_filter_index_mask(SearchMessagesFilter::Photo) |
             search_messages_filter_index_mask(SearchMessagesFilter::PhotoAndVideo);
    case MessageText::ID:
      for (auto &entity : static_cast<const MessageText *>(content)->text.entities) {
        if (entity.type == MessageEntity::Type::Url || entity.type == MessageEntity::Type::EmailAddress) {
          return search_messages_filter_index_mask(SearchMessagesFilter::Url);
        }
      }
      return 0;
    case MessageVideo::ID: {
      auto message_video = static_cast<const MessageVideo *>(content);
      auto duration = td_->videos_manager_->get_video_duration(message_video->file_id);
      return is_secret || duration > 0 ? search_messages_filter_index_mask(SearchMessagesFilter::Video) |
                                             search_messages_filter_index_mask(SearchMessagesFilter::PhotoAndVideo)
                                       : search_messages_filter_index_mask(SearchMessagesFilter::Document);
    }
    case MessageVideoNote::ID: {
      auto message_video_note = static_cast<const MessageVideoNote *>(content);
      auto duration = td_->video_notes_manager_->get_video_note_duration(message_video_note->file_id);
      return is_secret || duration > 0 ? search_messages_filter_index_mask(SearchMessagesFilter::VideoNote) |
                                             search_messages_filter_index_mask(SearchMessagesFilter::VoiceAndVideoNote)
                                       : search_messages_filter_index_mask(SearchMessagesFilter::Document);
    }
    case MessageVoiceNote::ID:
      return search_messages_filter_index_mask(SearchMessagesFilter::VoiceNote) |
             search_messages_filter_index_mask(SearchMessagesFilter::VoiceAndVideoNote);
    case MessageChatChangePhoto::ID:
      return search_messages_filter_index_mask(SearchMessagesFilter::ChatPhoto);
    case MessageCall::ID: {
      int32 index_mask = search_messages_filter_index_mask(SearchMessagesFilter::Call);
      auto message_call = static_cast<const MessageCall *>(content);
      if (!is_outgoing && (message_call->discard_reason == CallDiscardReason::Declined ||
                           message_call->discard_reason == CallDiscardReason::Missed)) {
        index_mask |= search_messages_filter_index_mask(SearchMessagesFilter::MissedCall);
      }
      return index_mask;
    }
    case MessageContact::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessageSticker::ID:
    case MessageUnsupported::ID:
    case MessageVenue::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return 0;
    default:
      UNREACHABLE();
      return 0;
  }
  return 0;
}

vector<MessageId> MessagesManager::get_message_ids(const vector<int64> &input_message_ids) {
  vector<MessageId> message_ids;
  message_ids.reserve(input_message_ids.size());
  for (auto &input_message_id : input_message_ids) {
    message_ids.push_back(MessageId(input_message_id));
  }
  return message_ids;
}

vector<int32> MessagesManager::get_server_message_ids(const vector<MessageId> &message_ids) {
  return transform(message_ids, [](MessageId message_id) { return message_id.get_server_message_id().get(); });
}

tl_object_ptr<telegram_api::InputMessage> MessagesManager::get_input_message(MessageId message_id) {
  return make_tl_object<telegram_api::inputMessageID>(message_id.get_server_message_id().get());
}

tl_object_ptr<telegram_api::InputPeer> MessagesManager::get_input_peer(DialogId dialog_id,
                                                                       AccessRights access_rights) const {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      UserId user_id = dialog_id.get_user_id();
      return td_->contacts_manager_->get_input_peer_user(user_id, access_rights);
    }
    case DialogType::Chat: {
      ChatId chat_id = dialog_id.get_chat_id();
      return td_->contacts_manager_->get_input_peer_chat(chat_id, access_rights);
    }
    case DialogType::Channel: {
      ChannelId channel_id = dialog_id.get_channel_id();
      return td_->contacts_manager_->get_input_peer_channel(channel_id, access_rights);
    }
    case DialogType::SecretChat:
      return nullptr;
    case DialogType::None:
      return make_tl_object<telegram_api::inputPeerEmpty>();
    default:
      UNREACHABLE();
      return nullptr;
  }
}

vector<tl_object_ptr<telegram_api::InputPeer>> MessagesManager::get_input_peers(const vector<DialogId> &dialog_ids,
                                                                                AccessRights access_rights) const {
  vector<tl_object_ptr<telegram_api::InputPeer>> input_peers;
  input_peers.reserve(dialog_ids.size());
  for (auto &dialog_id : dialog_ids) {
    auto input_peer = get_input_peer(dialog_id, access_rights);
    if (input_peer == nullptr) {
      LOG(ERROR) << "Have no access to " << dialog_id;
      continue;
    }
    input_peers.push_back(std::move(input_peer));
  }
  return input_peers;
}

tl_object_ptr<telegram_api::inputDialogPeer> MessagesManager::get_input_dialog_peer(DialogId dialog_id,
                                                                                    AccessRights access_rights) const {
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
    case DialogType::None:
      return make_tl_object<telegram_api::inputDialogPeer>(get_input_peer(dialog_id, access_rights));
    case DialogType::SecretChat:
      return nullptr;
    default:
      UNREACHABLE();
      return nullptr;
  }
}

vector<tl_object_ptr<telegram_api::inputDialogPeer>> MessagesManager::get_input_dialog_peers(
    const vector<DialogId> &dialog_ids, AccessRights access_rights) const {
  vector<tl_object_ptr<telegram_api::inputDialogPeer>> input_dialog_peers;
  input_dialog_peers.reserve(dialog_ids.size());
  for (auto &dialog_id : dialog_ids) {
    auto input_dialog_peer = get_input_dialog_peer(dialog_id, access_rights);
    if (input_dialog_peer == nullptr) {
      LOG(ERROR) << "Have no access to " << dialog_id;
      continue;
    }
    input_dialog_peers.push_back(std::move(input_dialog_peer));
  }
  return input_dialog_peers;
}

bool MessagesManager::have_input_peer(DialogId dialog_id, AccessRights access_rights) const {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      UserId user_id = dialog_id.get_user_id();
      return td_->contacts_manager_->have_input_peer_user(user_id, access_rights);
    }
    case DialogType::Chat: {
      ChatId chat_id = dialog_id.get_chat_id();
      return td_->contacts_manager_->have_input_peer_chat(chat_id, access_rights);
    }
    case DialogType::Channel: {
      ChannelId channel_id = dialog_id.get_channel_id();
      return td_->contacts_manager_->have_input_peer_channel(channel_id, access_rights);
    }
    case DialogType::SecretChat: {
      SecretChatId secret_chat_id = dialog_id.get_secret_chat_id();
      return td_->contacts_manager_->have_input_encrypted_peer(secret_chat_id, access_rights);
    }
    case DialogType::None:
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

bool MessagesManager::have_dialog_info(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      UserId user_id = dialog_id.get_user_id();
      return td_->contacts_manager_->have_user(user_id);
    }
    case DialogType::Chat: {
      ChatId chat_id = dialog_id.get_chat_id();
      return td_->contacts_manager_->have_chat(chat_id);
    }
    case DialogType::Channel: {
      ChannelId channel_id = dialog_id.get_channel_id();
      return td_->contacts_manager_->have_channel(channel_id);
    }
    case DialogType::SecretChat: {
      SecretChatId secret_chat_id = dialog_id.get_secret_chat_id();
      return td_->contacts_manager_->have_secret_chat(secret_chat_id);
    }
    case DialogType::None:
    default:
      return false;
  }
}

bool MessagesManager::have_dialog_info_force(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      UserId user_id = dialog_id.get_user_id();
      return td_->contacts_manager_->have_user_force(user_id);
    }
    case DialogType::Chat: {
      ChatId chat_id = dialog_id.get_chat_id();
      return td_->contacts_manager_->have_chat_force(chat_id);
    }
    case DialogType::Channel: {
      ChannelId channel_id = dialog_id.get_channel_id();
      return td_->contacts_manager_->have_channel_force(channel_id);
    }
    case DialogType::SecretChat: {
      SecretChatId secret_chat_id = dialog_id.get_secret_chat_id();
      return td_->contacts_manager_->have_secret_chat_force(secret_chat_id);
    }
    case DialogType::None:
    default:
      return false;
  }
}

tl_object_ptr<telegram_api::inputEncryptedChat> MessagesManager::get_input_encrypted_chat(
    DialogId dialog_id, AccessRights access_rights) const {
  switch (dialog_id.get_type()) {
    case DialogType::SecretChat: {
      SecretChatId secret_chat_id = dialog_id.get_secret_chat_id();
      return td_->contacts_manager_->get_input_encrypted_chat(secret_chat_id, access_rights);
    }
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
    case DialogType::None:
    default:
      UNREACHABLE();
      return nullptr;
  }
}

bool MessagesManager::is_allowed_useless_update(const tl_object_ptr<telegram_api::Update> &update) const {
  auto constructor_id = update->get_id();
  if (constructor_id == dummyUpdate::ID) {
    // allow dummyUpdate just in case
    return true;
  }
  if (constructor_id == telegram_api::updateNewMessage::ID) {
    auto message = static_cast<const telegram_api::updateNewMessage *>(update.get())->message_.get();
    if (message->get_id() == telegram_api::message::ID) {
      auto m = static_cast<const telegram_api::message *>(message);
      bool is_outgoing = (m->flags_ & MESSAGE_FLAG_IS_OUT) != 0 ||
                         UserId(m->from_id_) == td_->contacts_manager_->get_my_id("is_allowed_useless_update");
      if (is_outgoing && m->media_ != nullptr && m->media_->get_id() != telegram_api::messageMediaEmpty::ID) {
        // allow outgoing media, because they are returned if random_id coincided
        return true;
      }
    }
    if (message->get_id() == telegram_api::messageService::ID) {
      auto m = static_cast<const telegram_api::messageService *>(message);
      bool is_outgoing = (m->flags_ & MESSAGE_FLAG_IS_OUT) != 0 ||
                         UserId(m->from_id_) == td_->contacts_manager_->get_my_id("is_allowed_useless_update");
      if (is_outgoing && m->action_->get_id() == telegram_api::messageActionScreenshotTaken::ID) {
        // allow outgoing messageActionScreenshotTaken, because they are returned if random_id coincided
        return true;
      }
    }
  }

  return false;
}

bool MessagesManager::check_update_dialog_id(const tl_object_ptr<telegram_api::Update> &update, DialogId dialog_id) {
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      return true;
    case DialogType::Channel:
    case DialogType::SecretChat:
    case DialogType::None:
      LOG(ERROR) << "Receive update in wrong " << dialog_id << ": " << oneline(to_string(update));
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

void MessagesManager::skip_old_pending_update(tl_object_ptr<telegram_api::Update> &&update, int32 new_pts,
                                              int32 old_pts, int32 pts_count, const char *source) {
  if (update->get_id() == telegram_api::updateNewMessage::ID) {
    auto update_new_message = static_cast<telegram_api::updateNewMessage *>(update.get());
    auto full_message_id = get_full_message_id(update_new_message->message_);
    if (update_message_ids_.find(full_message_id) != update_message_ids_.end()) {
      if (new_pts == old_pts) {  // otherwise message can be already deleted
        // apply sent message anyway
        on_get_message(std::move(update_new_message->message_), true, false, true, true,
                       "updateNewMessage with an awaited message");
        return;
      } else {
        LOG(ERROR) << "Receive awaited sent " << full_message_id << " from " << source << " with pts " << new_pts
                   << " and pts_count " << pts_count << ", but current pts is " << old_pts;
        dump_debug_message_op(get_dialog(full_message_id.get_dialog_id()), 3);
      }
    }
  }
  if (update->get_id() == updateSentMessage::ID) {
    auto update_sent_message = static_cast<updateSentMessage *>(update.get());
    if (being_sent_messages_.count(update_sent_message->random_id_) > 0) {
      if (new_pts == old_pts) {  // otherwise message can be already deleted
        // apply sent message anyway
        on_send_message_success(update_sent_message->random_id_, update_sent_message->message_id_,
                                update_sent_message->date_, FileId(), "process old updateSentMessage");
        return;
      } else {
        LOG(ERROR) << "Receive awaited sent " << update_sent_message->message_id_ << " from " << source << " with pts "
                   << new_pts << " and pts_count " << pts_count << ", but current pts is " << old_pts;
        dump_debug_message_op(get_dialog(being_sent_messages_[update_sent_message->random_id_].get_dialog_id()), 3);
      }
    }
    return;
  }

  // very old or unuseful update
  LOG_IF(WARNING, new_pts == old_pts && pts_count == 0 && !is_allowed_useless_update(update))
      << "Receive useless update " << oneline(to_string(update)) << " from " << source;
}

void MessagesManager::add_pending_update(tl_object_ptr<telegram_api::Update> &&update, int32 new_pts, int32 pts_count,
                                         bool force_apply, const char *source) {
  // do not try to run getDifference from this function
  CHECK(update != nullptr);
  CHECK(source != nullptr);
  LOG(INFO) << "Receive from " << source << " pending " << to_string(update) << "new_pts = " << new_pts
            << ", pts_count = " << pts_count << ", force_apply = " << force_apply;
  if (pts_count < 0 || new_pts <= pts_count) {
    LOG(ERROR) << "Receive update with wrong pts = " << new_pts << " or pts_count = " << pts_count << " from " << source
               << ": " << oneline(to_string(update));
    return;
  }

  // TODO need to save all updates that can change result of running queries not associated with pts (for example
  // getHistory) and apply them to result of this queries

  switch (update->get_id()) {
    case dummyUpdate::ID:
    case updateSentMessage::ID:
    case telegram_api::updateReadMessagesContents::ID:
    case telegram_api::updateDeleteMessages::ID:
      // nothing to check
      break;
    case telegram_api::updateNewMessage::ID: {
      auto update_new_message = static_cast<const telegram_api::updateNewMessage *>(update.get());
      DialogId dialog_id = get_message_dialog_id(update_new_message->message_);
      if (!check_update_dialog_id(update, dialog_id)) {
        return;
      }
      break;
    }
    case telegram_api::updateReadHistoryInbox::ID: {
      auto update_read_history_inbox = static_cast<const telegram_api::updateReadHistoryInbox *>(update.get());
      auto dialog_id = DialogId(update_read_history_inbox->peer_);
      if (!check_update_dialog_id(update, dialog_id)) {
        return;
      }
      break;
    }
    case telegram_api::updateReadHistoryOutbox::ID: {
      auto update_read_history_outbox = static_cast<const telegram_api::updateReadHistoryOutbox *>(update.get());
      auto dialog_id = DialogId(update_read_history_outbox->peer_);
      if (!check_update_dialog_id(update, dialog_id)) {
        return;
      }
      break;
    }
    case telegram_api::updateEditMessage::ID: {
      auto update_edit_message = static_cast<const telegram_api::updateEditMessage *>(update.get());
      DialogId dialog_id = get_message_dialog_id(update_edit_message->message_);
      if (!check_update_dialog_id(update, dialog_id)) {
        return;
      }
      break;
    }
    default:
      LOG(ERROR) << "Receive unexpected update " << oneline(to_string(update)) << "from " << source;
      return;
  }

  if (force_apply) {
    CHECK(pending_updates_.empty());
    CHECK(accumulated_pts_ == -1);
    if (pts_count != 0) {
      LOG(ERROR) << "Receive forced update with pts_count = " << pts_count << " from " << source;
    }

    process_update(std::move(update));
    return;
  }
  if (DROP_UPDATES) {
    return set_get_difference_timeout(1.0);
  }

  int32 old_pts = td_->updates_manager_->get_pts();
  if (new_pts < old_pts - 19999) {
    // restore pts after delete_first_messages
    LOG(ERROR) << "Restore pts after delete_first_messages from " << old_pts << " to " << new_pts
               << " is temporarily disabled, pts_count = " << pts_count << ", update is from " << source << ": "
               << oneline(to_string(update));
    if (old_pts < 10000000 && update->get_id() == telegram_api::updateNewMessage::ID) {
      auto update_new_message = static_cast<telegram_api::updateNewMessage *>(update.get());
      auto dialog_id = get_message_dialog_id(update_new_message->message_);
      dump_debug_message_op(get_dialog(dialog_id), 6);
    }
    set_get_difference_timeout(0.001);

    /*
    LOG(WARNING) << "Restore pts after delete_first_messages";
    td_->updates_manager_->set_pts(new_pts - 1, "restore pts after delete_first_messages");
    old_pts = td_->updates_manager_->get_pts();
    CHECK(old_pts == new_pts - 1);
    */
  }

  if (new_pts <= old_pts) {
    skip_old_pending_update(std::move(update), new_pts, old_pts, pts_count, source);
    return;
  }

  if (td_->updates_manager_->running_get_difference()) {
    LOG(INFO) << "Save pending update got while running getDifference from " << source;
    CHECK(update->get_id() == dummyUpdate::ID || update->get_id() == updateSentMessage::ID);
    if (pts_count > 0) {
      postponed_pts_updates_.emplace(new_pts, PendingPtsUpdate(std::move(update), new_pts, pts_count));
    }
    return;
  }

  if (old_pts + pts_count > new_pts) {
    LOG(WARNING) << "old_pts (= " << old_pts << ") + pts_count (= " << pts_count << ") > new_pts (= " << new_pts
                 << "). Logged in " << G()->shared_config().get_option_integer("authorization_date") << ". Update from "
                 << source << " = " << oneline(to_string(update));
    set_get_difference_timeout(0.001);
    return;
  }

  accumulated_pts_count_ += pts_count;
  if (new_pts > accumulated_pts_) {
    accumulated_pts_ = new_pts;
  }

  if (old_pts + accumulated_pts_count_ > accumulated_pts_) {
    LOG(WARNING) << "old_pts (= " << old_pts << ") + accumulated_pts_count (= " << accumulated_pts_count_
                 << ") > accumulated_pts (= " << accumulated_pts_ << "). new_pts = " << new_pts
                 << ", pts_count = " << pts_count << ". Logged in "
                 << G()->shared_config().get_option_integer("authorization_date") << ". Update from " << source << " = "
                 << oneline(to_string(update));
    set_get_difference_timeout(0.001);
    return;
  }

  LOG_IF(INFO, pts_count == 0 && update->get_id() != dummyUpdate::ID) << "Skip useless update " << to_string(update);

  if (pending_updates_.empty() && old_pts + accumulated_pts_count_ == accumulated_pts_ &&
      !pts_gap_timeout_.has_timeout()) {
    if (pts_count > 0) {
      process_update(std::move(update));

      td_->updates_manager_->set_pts(accumulated_pts_, "process pending updates fast path")
          .set_value(Unit());  // TODO can't set until get messages really stored on persistent storage
      accumulated_pts_count_ = 0;
      accumulated_pts_ = -1;
    }
    return;
  }

  if (pts_count > 0) {
    pending_updates_.emplace(new_pts, PendingPtsUpdate(std::move(update), new_pts, pts_count));
  }

  if (old_pts + accumulated_pts_count_ < accumulated_pts_) {
    set_get_difference_timeout(UpdatesManager::MAX_UNFILLED_GAP_TIME);
    return;
  }

  CHECK(old_pts + accumulated_pts_count_ == accumulated_pts_);
  if (!pending_updates_.empty()) {
    process_pending_updates();
  }
}

MessagesManager::Dialog *MessagesManager::get_service_notifications_dialog() {
  UserId service_notifications_user_id(777000);
  if (!td_->contacts_manager_->have_user_force(service_notifications_user_id) ||
      !td_->contacts_manager_->have_user(service_notifications_user_id)) {
    auto user = telegram_api::make_object<telegram_api::user>(
        131127, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
        false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
        false /*ignored*/, 777000, 1, "Telegram", "Updates", string(), "42777",
        telegram_api::make_object<telegram_api::userProfilePhoto>(
            3337190045231018,
            telegram_api::make_object<telegram_api::fileLocation>(1, 702229962, 26779, 5859320227133863146),
            telegram_api::make_object<telegram_api::fileLocation>(1, 702229962, 26781, -3695031185685824216)),
        nullptr, 0, string(), string(), string());
    td_->contacts_manager_->on_get_user(std::move(user));
  }
  DialogId service_notifications_dialog_id(service_notifications_user_id);
  force_create_dialog(service_notifications_dialog_id, "get_service_notifications_dialog");
  return get_dialog(service_notifications_dialog_id);
}

void MessagesManager::on_update_service_notification(tl_object_ptr<telegram_api::updateServiceNotification> &&update) {
  int32 ttl = 0;
  auto content = get_message_content(
      get_message_text(std::move(update->message_), std::move(update->entities_), update->inbox_date_),
      std::move(update->media_),
      td_->auth_manager_->is_bot() ? DialogId() : get_service_notifications_dialog()->dialog_id, false, UserId(), &ttl);
  bool is_content_secret = is_secret_message_content(ttl, content->get_id());
  if ((update->flags_ & telegram_api::updateServiceNotification::POPUP_MASK) != 0) {
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateServiceNotification>(
                     update->type_, get_message_content_object(content.get(), update->inbox_date_, is_content_secret)));
  }
  if ((update->flags_ & telegram_api::updateServiceNotification::INBOX_DATE_MASK) != 0 &&
      !td_->auth_manager_->is_bot()) {
    Dialog *d = get_service_notifications_dialog();
    CHECK(d != nullptr);
    auto dialog_id = d->dialog_id;
    CHECK(dialog_id.get_type() == DialogType::User);
    auto new_message = make_unique<Message>();
    new_message->message_id = get_next_local_message_id(d);
    new_message->random_y = get_random_y(new_message->message_id);
    new_message->sender_user_id = dialog_id.get_user_id();
    new_message->date = update->inbox_date_;
    new_message->ttl = ttl;
    new_message->is_content_secret = is_content_secret;
    new_message->content = std::move(content);
    new_message->have_previous = true;
    new_message->have_next = true;

    bool need_update = true;
    bool need_update_dialog_pos = false;

    Message *m = add_message_to_dialog(d, std::move(new_message), true, &need_update, &need_update_dialog_pos,
                                       "on_update_service_notification");
    if (m != nullptr && need_update) {
      send_update_new_message(d, m);
    }

    if (need_update_dialog_pos) {
      send_update_chat_last_message(d, "on_update_service_notification");
    }
  }
}

void MessagesManager::on_update_contact_registered(tl_object_ptr<telegram_api::updateContactRegistered> &&update) {
  if (update->date_ <= 0) {
    LOG(ERROR) << "Receive wrong date " << update->date_ << " in updateContactRegistered";
    return;
  }
  UserId user_id(update->user_id_);
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id << " in updateContactRegistered";
    return;
  }

  if (!td_->auth_manager_->is_bot() &&
      !G()->shared_config().get_option_boolean("disable_contact_registered_notifications")) {
    DialogId dialog_id(user_id);
    force_create_dialog(dialog_id, "on_update_contact_registered");
    Dialog *d = get_dialog(dialog_id);
    CHECK(d != nullptr);

    if (d->has_contact_registered_message) {
      LOG(INFO) << "Ignore duplicate updateContactRegistered about " << user_id;
      return;
    }
    if (d->last_message_id.is_valid()) {
      auto m = get_message(d, d->last_message_id);
      CHECK(m != nullptr);
      if (m->content->get_id() == MessageContactRegistered::ID) {
        LOG(INFO) << "Ignore duplicate updateContactRegistered about " << user_id;
        return;
      }
    }

    auto new_message = make_unique<Message>();
    new_message->message_id = get_next_local_message_id(d);
    new_message->random_y = get_random_y(new_message->message_id);
    new_message->sender_user_id = user_id;
    new_message->date = update->date_;
    new_message->content = make_unique<MessageContactRegistered>();
    new_message->have_previous = true;
    new_message->have_next = true;

    bool need_update = true;
    bool need_update_dialog_pos = false;

    Message *m = add_message_to_dialog(d, std::move(new_message), true, &need_update, &need_update_dialog_pos,
                                       "on_update_contact_registered");
    if (m != nullptr && need_update) {
      send_update_new_message(d, m);
    }

    if (need_update_dialog_pos) {
      send_update_chat_last_message(d, "on_update_contact_registered");
    }
  }
}

void MessagesManager::on_update_new_channel_message(tl_object_ptr<telegram_api::updateNewChannelMessage> &&update) {
  int new_pts = update->pts_;
  int pts_count = update->pts_count_;
  DialogId dialog_id = get_message_dialog_id(update->message_);
  switch (dialog_id.get_type()) {
    case DialogType::None:
      return;
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::SecretChat:
      LOG(ERROR) << "Receive updateNewChannelMessage in wrong " << dialog_id;
      return;
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      if (!td_->contacts_manager_->have_channel(channel_id)) {
        // if min channel was received
        if (td_->contacts_manager_->have_min_channel(channel_id)) {
          td_->updates_manager_->schedule_get_difference("on_update_new_channel_message");
          return;
        }
      }
      // Ok
      break;
    }
    default:
      UNREACHABLE();
      return;
  }

  if (pts_count < 0 || new_pts <= pts_count) {
    LOG(ERROR) << "Receive new channel message with wrong pts = " << new_pts << " or pts_count = " << pts_count << ": "
               << oneline(to_string(update));
    return;
  }

  add_pending_channel_update(dialog_id, std::move(update), new_pts, pts_count, "on_update_new_channel_message");
}

void MessagesManager::on_update_edit_channel_message(tl_object_ptr<telegram_api::updateEditChannelMessage> &&update) {
  int new_pts = update->pts_;
  int pts_count = update->pts_count_;
  DialogId dialog_id = get_message_dialog_id(update->message_);
  switch (dialog_id.get_type()) {
    case DialogType::None:
      return;
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::SecretChat:
      LOG(ERROR) << "Receive updateNewChannelMessage in wrong " << dialog_id;
      return;
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      if (!td_->contacts_manager_->have_channel(channel_id)) {
        // if min channel was received
        if (td_->contacts_manager_->have_min_channel(channel_id)) {
          td_->updates_manager_->schedule_get_difference("on_update_edit_channel_message");
          return;
        }
      }
      // Ok
      break;
    }
    default:
      UNREACHABLE();
      return;
  }

  if (pts_count < 0 || new_pts <= pts_count) {
    LOG(ERROR) << "Receive edited channel message with wrong pts = " << new_pts << " or pts_count = " << pts_count
               << ": " << oneline(to_string(update));
    return;
  }

  add_pending_channel_update(dialog_id, std::move(update), new_pts, pts_count, "on_update_edit_channel_message");
}

void MessagesManager::on_update_read_channel_inbox(tl_object_ptr<telegram_api::updateReadChannelInbox> &&update) {
  ChannelId channel_id(update->channel_id_);
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id << " in updateReadChannelInbox";
    return;
  }

  DialogId dialog_id = DialogId(channel_id);
  read_history_inbox(dialog_id, MessageId(ServerMessageId(update->max_id_)), -1, "updateReadChannelInbox");
}

void MessagesManager::on_update_read_channel_outbox(tl_object_ptr<telegram_api::updateReadChannelOutbox> &&update) {
  ChannelId channel_id(update->channel_id_);
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id << " in updateReadChannelOutbox";
    return;
  }

  DialogId dialog_id = DialogId(channel_id);
  read_history_outbox(dialog_id, MessageId(ServerMessageId(update->max_id_)));
}

void MessagesManager::on_update_read_channel_messages_contents(
    tl_object_ptr<telegram_api::updateChannelReadMessagesContents> &&update) {
  ChannelId channel_id(update->channel_id_);
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id << " in updateChannelReadMessagesContents";
    return;
  }

  DialogId dialog_id = DialogId(channel_id);

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(INFO) << "Receive read channel messages contents update in unknown " << dialog_id;
    return;
  }

  for (auto &server_message_id : update->messages_) {
    read_channel_message_content_from_updates(d, MessageId(ServerMessageId(server_message_id)));
  }
}

void MessagesManager::on_update_channel_too_long(tl_object_ptr<telegram_api::updateChannelTooLong> &&update,
                                                 bool force_apply) {
  ChannelId channel_id(update->channel_id_);
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id << " in updateChannelTooLong";
    return;
  }

  DialogId dialog_id = DialogId(channel_id);
  auto d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    auto pts = load_channel_pts(dialog_id);
    if (pts > 0) {
      d = add_dialog(dialog_id);
      CHECK(d != nullptr);
      CHECK(d->pts == pts);
      update_dialog_pos(d, false, "on_update_channel_too_long");
    }
  }

  int32 update_pts = (update->flags_ & UPDATE_CHANNEL_TO_LONG_FLAG_HAS_PTS) ? update->pts_ : 0;

  if (force_apply) {
    if (d == nullptr) {
      get_channel_difference(dialog_id, -1, true, "on_update_channel_too_long 1");
    } else {
      get_channel_difference(dialog_id, d->pts, true, "on_update_channel_too_long 2");
    }
  } else {
    if (d == nullptr) {
      td_->updates_manager_->schedule_get_difference("on_update_channel_too_long");
    } else if (update_pts > d->pts) {
      get_channel_difference(dialog_id, d->pts, true, "on_update_channel_too_long 3");
    }
  }
}

void MessagesManager::on_update_message_views(FullMessageId full_message_id, int32 views) {
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(INFO) << "Ignore updateMessageViews in unknown " << dialog_id;
    return;
  }
  auto message_id = full_message_id.get_message_id();
  Message *m = get_message_force(d, message_id);
  if (m == nullptr) {
    LOG(INFO) << "Ignore updateMessageViews about unknown " << full_message_id;
    return;
  }

  if (update_message_views(full_message_id.get_dialog_id(), m, views)) {
    on_message_changed(d, m, "on_update_message_views");
  }
}

bool MessagesManager::update_message_views(DialogId dialog_id, Message *m, int32 views) {
  CHECK(m != nullptr);
  if (views > m->views) {
    m->views = views;
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateMessageViews>(dialog_id.get(), m->message_id.get(), m->views));
    return true;
  }
  return false;
}

bool MessagesManager::update_message_contains_unread_mention(Dialog *d, Message *m, bool contains_unread_mention,
                                                             const char *source) {
  CHECK(m != nullptr) << source;
  if (!contains_unread_mention && m->contains_unread_mention) {
    m->contains_unread_mention = false;
    if (d->unread_mention_count == 0) {
      LOG_IF(ERROR, d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] != -1)
          << "Unread mention count of " << d->dialog_id << " became negative from " << source;
    } else {
      d->unread_mention_count--;
      d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] =
          d->unread_mention_count;
      on_dialog_updated(d->dialog_id, "update_message_contains_unread_mention");
    }
    LOG(INFO) << "Update unread mention message count in " << d->dialog_id << " to " << d->unread_mention_count
              << " by reading " << m->message_id << " from " << source;

    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateMessageMentionRead>(d->dialog_id.get(), m->message_id.get(),
                                                                  d->unread_mention_count));
    return true;
  }
  return false;
}

void MessagesManager::on_read_channel_inbox(ChannelId channel_id, MessageId max_message_id, int32 server_unread_count) {
  DialogId dialog_id(channel_id);
  if (max_message_id.is_valid() || server_unread_count > 0) {
    /*
    // dropping unread count can make things worse, so don't drop it
    if (server_unread_count > 0 && G()->parameters().use_message_db) {
      const Dialog *d = get_dialog_force(dialog_id);
      if (d == nullptr) {
        return;
      }

      if (d->is_last_read_inbox_message_id_inited) {
        server_unread_count = -1;
      }
    }
    */
    read_history_inbox(dialog_id, max_message_id, server_unread_count, "on_read_channel_inbox");
  }
}

void MessagesManager::on_read_channel_outbox(ChannelId channel_id, MessageId max_message_id) {
  DialogId dialog_id(channel_id);
  if (max_message_id.is_valid()) {
    read_history_outbox(dialog_id, max_message_id);
  }
}

void MessagesManager::on_update_channel_max_unavailable_message_id(ChannelId channel_id,
                                                                   MessageId max_unavailable_message_id) {
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive max_unavailable_message_id in invalid " << channel_id;
    return;
  }

  DialogId dialog_id(channel_id);
  if (!max_unavailable_message_id.is_valid() && max_unavailable_message_id != MessageId()) {
    LOG(ERROR) << "Receive wrong max_unavailable_message_id: " << max_unavailable_message_id;
    max_unavailable_message_id = MessageId();
  }
  set_dialog_max_unavailable_message_id(dialog_id, max_unavailable_message_id, true,
                                        "on_update_channel_max_unavailable_message_id");
}

bool MessagesManager::need_cancel_user_dialog_action(int32 action_id, int32 message_content_id) {
  if (message_content_id == -1) {
    return true;
  }

  if (action_id == td_api::chatActionTyping::ID) {
    return message_content_id == MessageText::ID || message_content_id == MessageGame::ID ||
           can_have_message_content_caption(message_content_id);
  }

  switch (message_content_id) {
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageDocument::ID:
      return action_id == td_api::chatActionUploadingDocument::ID;
    case MessageExpiredPhoto::ID:
    case MessagePhoto::ID:
      return action_id == td_api::chatActionUploadingPhoto::ID;
    case MessageExpiredVideo::ID:
    case MessageVideo::ID:
      return action_id == td_api::chatActionRecordingVideo::ID || action_id == td_api::chatActionUploadingVideo::ID;
    case MessageVideoNote::ID:
      return action_id == td_api::chatActionRecordingVideoNote::ID ||
             action_id == td_api::chatActionUploadingVideoNote::ID;
    case MessageVoiceNote::ID:
      return action_id == td_api::chatActionRecordingVoiceNote::ID ||
             action_id == td_api::chatActionUploadingVoiceNote::ID;
    case MessageContact::ID:
      return action_id == td_api::chatActionChoosingContact::ID;
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessageVenue::ID:
      return action_id == td_api::chatActionChoosingLocation::ID;
    case MessageText::ID:
    case MessageGame::ID:
    case MessageUnsupported::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

void MessagesManager::on_user_dialog_action(DialogId dialog_id, UserId user_id,
                                            tl_object_ptr<td_api::ChatAction> &&action, int32 message_content_id) {
  if (td_->auth_manager_->is_bot() || !user_id.is_valid() || is_broadcast_channel(dialog_id)) {
    return;
  }

  bool is_canceled = action == nullptr || action->get_id() == td_api::chatActionCancel::ID;
  if (is_canceled) {
    auto actions_it = active_dialog_actions_.find(dialog_id);
    if (actions_it == active_dialog_actions_.end()) {
      return;
    }

    auto &active_actions = actions_it->second;
    auto it = std::find_if(active_actions.begin(), active_actions.end(),
                           [user_id](const ActiveDialogAction &action) { return action.user_id == user_id; });
    if (it == active_actions.end()) {
      return;
    }

    if (!need_cancel_user_dialog_action(it->action_id, message_content_id)) {
      return;
    }

    LOG(DEBUG) << "Cancel action of " << user_id << " in " << dialog_id;
    active_actions.erase(it);
    if (active_actions.empty()) {
      active_dialog_actions_.erase(dialog_id);
      LOG(DEBUG) << "Cancel action timeout in " << dialog_id;
      active_dialog_action_timeout_.cancel_timeout(dialog_id.get());
    }
    if (action == nullptr) {
      action = make_tl_object<td_api::chatActionCancel>();
    }
  } else {
    auto &active_actions = active_dialog_actions_[dialog_id];
    auto it = std::find_if(active_actions.begin(), active_actions.end(),
                           [user_id](const ActiveDialogAction &action) { return action.user_id == user_id; });
    int32 prev_action_id = 0;
    int32 prev_progress = 0;
    if (it != active_actions.end()) {
      LOG(DEBUG) << "Re-add action of " << user_id << " in " << dialog_id;
      prev_action_id = it->action_id;
      prev_progress = it->progress;
      active_actions.erase(it);
    } else {
      LOG(DEBUG) << "Add action of " << user_id << " in " << dialog_id;
    }

    auto action_id = action->get_id();
    auto progress = [&] {
      switch (action_id) {
        case td_api::chatActionUploadingVideo::ID:
          return static_cast<td_api::chatActionUploadingVideo &>(*action).progress_;
        case td_api::chatActionUploadingVoiceNote::ID:
          return static_cast<td_api::chatActionUploadingVoiceNote &>(*action).progress_;
        case td_api::chatActionUploadingPhoto::ID:
          return static_cast<td_api::chatActionUploadingPhoto &>(*action).progress_;
        case td_api::chatActionUploadingDocument::ID:
          return static_cast<td_api::chatActionUploadingDocument &>(*action).progress_;
        case td_api::chatActionUploadingVideoNote::ID:
          return static_cast<td_api::chatActionUploadingVideoNote &>(*action).progress_;
        default:
          return 0;
      }
    }();
    active_actions.emplace_back(user_id, action_id, Time::now());
    if (action_id == prev_action_id && progress <= prev_progress) {
      return;
    }
    if (active_actions.size() == 1u) {
      LOG(DEBUG) << "Set action timeout in " << dialog_id;
      active_dialog_action_timeout_.set_timeout_in(dialog_id.get(), DIALOG_ACTION_TIMEOUT);
    }
  }

  LOG(DEBUG) << "Send action of " << user_id << " in " << dialog_id << ": " << to_string(action);
  send_closure(G()->td(), &Td::send_update,
               make_tl_object<td_api::updateUserChatAction>(
                   dialog_id.get(), td_->contacts_manager_->get_user_id_object(user_id, "on_user_dialog_action"),
                   std::move(action)));
}

void MessagesManager::cancel_user_dialog_action(DialogId dialog_id, const Message *m) {
  CHECK(m != nullptr);
  if (m->forward_info != nullptr || m->via_bot_user_id.is_valid() || m->is_channel_post) {
    return;
  }

  on_user_dialog_action(dialog_id, m->sender_user_id, nullptr, m->content->get_id());
}

void MessagesManager::add_pending_channel_update(DialogId dialog_id, tl_object_ptr<telegram_api::Update> &&update,
                                                 int32 new_pts, int32 pts_count, const char *source,
                                                 bool is_postponed_udpate) {
  LOG(INFO) << "Receive from " << source << " pending " << to_string(update);
  CHECK(update != nullptr);
  CHECK(dialog_id.get_type() == DialogType::Channel);
  if (pts_count < 0 || new_pts <= pts_count) {
    LOG(ERROR) << "Receive channel update from " << source << " with wrong pts = " << new_pts
               << " or pts_count = " << pts_count << ": " << oneline(to_string(update));
    return;
  }

  // TODO need to save all updates that can change result of running queries not associated with pts (for example
  // getHistory) and apply them to result of this queries

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    auto pts = load_channel_pts(dialog_id);
    if (pts > 0) {
      auto channel_id = dialog_id.get_channel_id();
      if (!td_->contacts_manager_->have_channel(channel_id)) {
        // do not create dialog if there is no info about the channel
        return;
      }

      d = add_dialog(dialog_id);
      CHECK(d != nullptr);
      CHECK(d->pts == pts);
      update_dialog_pos(d, false, "add_pending_channel_update");
    }
  }
  if (d == nullptr) {
    // if there is no dialog, it can be created by the update
    LOG(INFO) << "Receive pending update from " << source << " about unknown " << dialog_id;
    if (running_get_channel_difference(dialog_id)) {
      return;
    }
  } else {
    int32 old_pts = d->pts;
    if (new_pts <= old_pts) {  // very old or unuseful update
      if (new_pts < old_pts - 19999 && !is_postponed_udpate) {
        // restore channel pts after delete_first_messages
        LOG(ERROR) << "Restore pts in " << d->dialog_id << " from " << source << " after delete_first_messages from "
                   << old_pts << " to " << new_pts << " is temporarily disabled, pts_count = " << pts_count
                   << ", update is from " << source << ": " << oneline(to_string(update));
        if (old_pts < 10000000) {
          dump_debug_message_op(d, 6);
        }
        get_channel_difference(dialog_id, old_pts, true, "add_pending_channel_update old");
      }

      if (update->get_id() == telegram_api::updateNewChannelMessage::ID) {
        auto update_new_channel_message = static_cast<telegram_api::updateNewChannelMessage *>(update.get());
        auto message_id = get_message_id(update_new_channel_message->message_);
        FullMessageId full_message_id(dialog_id, message_id);
        if (update_message_ids_.find(full_message_id) != update_message_ids_.end()) {
          // apply sent channel message
          on_get_message(std::move(update_new_channel_message->message_), true, true, true, true,
                         "updateNewChannelMessage with an awaited message");
          return;
        }
      }
      if (update->get_id() == updateSentMessage::ID) {
        auto update_sent_message = static_cast<updateSentMessage *>(update.get());
        if (being_sent_messages_.count(update_sent_message->random_id_) > 0) {
          // apply sent channel message
          on_send_message_success(update_sent_message->random_id_, update_sent_message->message_id_,
                                  update_sent_message->date_, FileId(), "process old updateSentChannelMessage");
          return;
        }
      }

      LOG_IF(WARNING, new_pts == old_pts && pts_count == 0)
          << "Receive from " << source << " useless channel update " << oneline(to_string(update));
      return;
    }

    if (running_get_channel_difference(dialog_id)) {
      if (pts_count > 0) {
        d->postponed_channel_updates.emplace(new_pts, PendingPtsUpdate(std::move(update), new_pts, pts_count));
      }
      return;
    }

    if (old_pts + pts_count != new_pts) {
      LOG(WARNING) << "Found a gap in the " << dialog_id << " with pts = " << old_pts << ". new_pts = " << new_pts
                   << ", pts_count = " << pts_count << " in update from " << source;

      if (pts_count > 0) {
        d->postponed_channel_updates.emplace(new_pts, PendingPtsUpdate(std::move(update), new_pts, pts_count));
      }

      get_channel_difference(dialog_id, old_pts, true, "add_pending_channel_update pts mismatch");
      return;
    }
  }

  if (d == nullptr || pts_count > 0) {
    process_channel_update(std::move(update));
    CHECK(!running_get_channel_difference(dialog_id)) << '"' << active_get_channel_differencies_[dialog_id] << '"';
  } else {
    LOG_IF(INFO, update->get_id() != dummyUpdate::ID)
        << "Skip useless channel update from " << source << ": " << to_string(update);
  }

  if (d == nullptr) {
    d = get_dialog(dialog_id);
    if (d == nullptr) {
      // dialog was not created by the update
      return;
    }
  }

  CHECK(new_pts > d->pts);
  set_channel_pts(d, new_pts, source);
}

void MessagesManager::set_get_difference_timeout(double timeout) {
  if (!pts_gap_timeout_.has_timeout()) {
    LOG(INFO) << "Gap in pts has found, current pts is " << td_->updates_manager_->get_pts();
    pts_gap_timeout_.set_callback(std::move(UpdatesManager::fill_pts_gap));
    pts_gap_timeout_.set_callback_data(static_cast<void *>(td_));
    pts_gap_timeout_.set_timeout_in(timeout);
  }
}

void MessagesManager::process_update(tl_object_ptr<telegram_api::Update> &&update) {
  switch (update->get_id()) {
    case dummyUpdate::ID:
      LOG(INFO) << "Process dummyUpdate";
      break;
    case telegram_api::updateNewMessage::ID:
      LOG(INFO) << "Process updateNewMessage";
      on_get_message(std::move(move_tl_object_as<telegram_api::updateNewMessage>(update)->message_), true, false, true,
                     true, "updateNewMessage");
      break;
    case updateSentMessage::ID: {
      auto send_message_success_update = move_tl_object_as<updateSentMessage>(update);
      LOG(INFO) << "Process updateSentMessage " << send_message_success_update->random_id_;
      on_send_message_success(send_message_success_update->random_id_, send_message_success_update->message_id_,
                              send_message_success_update->date_, FileId(), "process updateSentMessage");
      break;
    }
    case telegram_api::updateReadMessagesContents::ID: {
      auto read_contents_update = move_tl_object_as<telegram_api::updateReadMessagesContents>(update);
      LOG(INFO) << "Process updateReadMessageContents";
      for (auto &message_id : read_contents_update->messages_) {
        read_message_content_from_updates(MessageId(ServerMessageId(message_id)));
      }
      break;
    }
    case telegram_api::updateEditMessage::ID: {
      auto full_message_id =
          on_get_message(std::move(move_tl_object_as<telegram_api::updateEditMessage>(update)->message_), false, false,
                         false, false, "updateEditMessage");
      LOG(INFO) << "Process updateEditMessage";
      if (full_message_id != FullMessageId() && td_->auth_manager_->is_bot()) {
        send_update_message_edited(full_message_id);
      }
      break;
    }
    case telegram_api::updateDeleteMessages::ID: {
      auto delete_update = move_tl_object_as<telegram_api::updateDeleteMessages>(update);
      LOG(INFO) << "Process updateDeleteMessages";
      vector<MessageId> message_ids;
      for (auto &message : delete_update->messages_) {
        message_ids.push_back(MessageId(ServerMessageId(message)));
      }
      delete_messages_from_updates(message_ids);
      break;
    }
    case telegram_api::updateReadHistoryInbox::ID: {
      auto read_update = move_tl_object_as<telegram_api::updateReadHistoryInbox>(update);
      LOG(INFO) << "Process updateReadHistoryInbox";
      read_history_inbox(DialogId(read_update->peer_), MessageId(ServerMessageId(read_update->max_id_)), -1,
                         "updateReadHistoryInbox");
      break;
    }
    case telegram_api::updateReadHistoryOutbox::ID: {
      auto read_update = move_tl_object_as<telegram_api::updateReadHistoryOutbox>(update);
      LOG(INFO) << "Process updateReadHistoryOutbox";
      read_history_outbox(DialogId(read_update->peer_), MessageId(ServerMessageId(read_update->max_id_)));
      break;
    }
    default:
      UNREACHABLE();
  }
  CHECK(!td_->updates_manager_->running_get_difference());
}

void MessagesManager::process_channel_update(tl_object_ptr<telegram_api::Update> &&update) {
  switch (update->get_id()) {
    case dummyUpdate::ID:
      LOG(INFO) << "Process dummyUpdate";
      break;
    case updateSentMessage::ID: {
      auto send_message_success_update = move_tl_object_as<updateSentMessage>(update);
      LOG(INFO) << "Process updateSentMessage " << send_message_success_update->random_id_;
      on_send_message_success(send_message_success_update->random_id_, send_message_success_update->message_id_,
                              send_message_success_update->date_, FileId(), "process updateSentChannelMessage");
      break;
    }
    case telegram_api::updateNewChannelMessage::ID:
      LOG(INFO) << "Process updateNewChannelMessage";
      on_get_message(std::move(move_tl_object_as<telegram_api::updateNewChannelMessage>(update)->message_), true, true,
                     true, true, "updateNewChannelMessage");
      break;
    case telegram_api::updateDeleteChannelMessages::ID: {
      auto delete_channel_messages_update = move_tl_object_as<telegram_api::updateDeleteChannelMessages>(update);
      LOG(INFO) << "Process updateDeleteChannelMessages";
      ChannelId channel_id(delete_channel_messages_update->channel_id_);
      if (!channel_id.is_valid()) {
        LOG(ERROR) << "Receive invalid " << channel_id;
        break;
      }

      vector<MessageId> message_ids;
      for (auto &message : delete_channel_messages_update->messages_) {
        message_ids.push_back(MessageId(ServerMessageId(message)));
      }

      auto dialog_id = DialogId(channel_id);
      delete_dialog_messages_from_updates(dialog_id, message_ids);
      break;
    }
    case telegram_api::updateEditChannelMessage::ID: {
      auto full_message_id =
          on_get_message(std::move(move_tl_object_as<telegram_api::updateEditChannelMessage>(update)->message_), false,
                         true, false, false, "updateEditChannelMessage");
      LOG(INFO) << "Process updateEditChannelMessage";
      if (full_message_id != FullMessageId() && td_->auth_manager_->is_bot()) {
        send_update_message_edited(full_message_id);
      }
      break;
    }
    default:
      UNREACHABLE();
  }
}

void MessagesManager::process_pending_updates() {
  for (auto &update : pending_updates_) {
    process_update(std::move(update.second.update));
  }

  td_->updates_manager_->set_pts(accumulated_pts_, "process pending updates")
      .set_value(Unit());  // TODO can't set until get messages really stored on persistent storage
  drop_pending_updates();
}

void MessagesManager::drop_pending_updates() {
  accumulated_pts_count_ = 0;
  accumulated_pts_ = -1;
  pts_gap_timeout_.cancel_timeout();
  pending_updates_.clear();
}

NotificationSettingsScope MessagesManager::get_notification_settings_scope(
    tl_object_ptr<telegram_api::NotifyPeer> &&notify_peer_ptr) const {
  switch (notify_peer_ptr->get_id()) {
    case telegram_api::notifyPeer::ID: {
      auto notify_peer = move_tl_object_as<telegram_api::notifyPeer>(notify_peer_ptr);
      return DialogId(notify_peer->peer_).get();
    }
    case telegram_api::notifyUsers::ID:
      return NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS;
    case telegram_api::notifyChats::ID:
      return NOTIFICATION_SETTINGS_FOR_GROUP_CHATS;
    case telegram_api::notifyAll::ID:
      return NOTIFICATION_SETTINGS_FOR_ALL_CHATS;
    default:
      UNREACHABLE();
      return 0;
  }
}

NotificationSettingsScope MessagesManager::get_notification_settings_scope(
    const tl_object_ptr<td_api::NotificationSettingsScope> &scope) const {
  if (scope == nullptr) {
    return NOTIFICATION_SETTINGS_FOR_ALL_CHATS;
  }
  int32 scope_id = scope->get_id();
  switch (scope_id) {
    case td_api::notificationSettingsScopeChat::ID:
      return static_cast<const td_api::notificationSettingsScopeChat *>(scope.get())->chat_id_;
    case td_api::notificationSettingsScopePrivateChats::ID:
      return NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS;
    case td_api::notificationSettingsScopeBasicGroupChats::ID:
      return NOTIFICATION_SETTINGS_FOR_GROUP_CHATS;
    case td_api::notificationSettingsScopeAllChats::ID:
      return NOTIFICATION_SETTINGS_FOR_ALL_CHATS;
    default:
      UNREACHABLE();
      return 0;
  }
}

string MessagesManager::get_notification_settings_scope_database_key(NotificationSettingsScope scope) {
  switch (scope) {
    case NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS:
      return "nsfpc";
    case NOTIFICATION_SETTINGS_FOR_GROUP_CHATS:
      return "nsfgc";
    case NOTIFICATION_SETTINGS_FOR_ALL_CHATS:
      return "nsfac";
    default:
      UNREACHABLE();
      return "";
  }
}

bool MessagesManager::update_notification_settings(NotificationSettingsScope scope,
                                                   NotificationSettings *current_settings,
                                                   const NotificationSettings &new_settings) {
  bool need_update = current_settings->mute_until != new_settings.mute_until ||
                     current_settings->sound != new_settings.sound ||
                     current_settings->show_preview != new_settings.show_preview ||
                     current_settings->is_synchronized != new_settings.is_synchronized;
  bool is_changed = need_update || current_settings->silent_send_message != new_settings.silent_send_message;

  if (is_changed) {
    if (scope != NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS && scope != NOTIFICATION_SETTINGS_FOR_GROUP_CHATS &&
        scope != NOTIFICATION_SETTINGS_FOR_ALL_CHATS) {
      DialogId dialog_id(scope);
      CHECK(dialog_id.is_valid());
      Dialog *d = get_dialog(dialog_id);
      CHECK(d != nullptr) << "Wrong " << dialog_id << " in update_notification_settings";
      update_dialog_unmute_timeout(d, current_settings->mute_until, new_settings.mute_until);
      on_dialog_updated(dialog_id, "update_notification_settings");
    } else {
      string key = get_notification_settings_scope_database_key(scope);
      G()->td_db()->get_binlog_pmc()->set(key, log_event_store(new_settings).as_slice().str());
    }
    LOG(INFO) << "Update notification settings in " << scope << " from " << *current_settings << " to " << new_settings;
    *current_settings = new_settings;

    if (need_update) {
      send_closure(
          G()->td(), &Td::send_update,
          make_tl_object<td_api::updateNotificationSettings>(get_notification_settings_scope_object(scope),
                                                             get_notification_settings_object(current_settings)));
    }
  }
  return is_changed;
}

void MessagesManager::update_dialog_unmute_timeout(Dialog *d, int32 old_mute_until, int32 new_mute_until) {
  if (old_mute_until == new_mute_until) {
    return;
  }
  CHECK(d != nullptr);

  auto now = G()->unix_time_cached();
  if (new_mute_until >= now && new_mute_until < now + 366 * 86400) {
    dialog_unmute_timeout_.set_timeout_in(d->dialog_id.get(), new_mute_until - now + 1);
  } else {
    dialog_unmute_timeout_.cancel_timeout(d->dialog_id.get());
  }

  if (old_mute_until != -1 && is_unread_count_inited_ && d->order != DEFAULT_ORDER) {
    auto unread_count = d->server_unread_count + d->local_unread_count;
    if (unread_count != 0) {
      if (old_mute_until != 0 && new_mute_until == 0) {
        unread_message_muted_count_ -= unread_count;
        send_update_unread_message_count(d->dialog_id, true, "on_dialog_unmute");
      }
      if (old_mute_until == 0 && new_mute_until != 0) {
        unread_message_muted_count_ += unread_count;
        send_update_unread_message_count(d->dialog_id, true, "on_dialog_mute");
      }
    }
  }
}

void MessagesManager::on_dialog_unmute(DialogId dialog_id) {
  auto d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  if (d->notification_settings.mute_until == 0) {
    return;
  }

  auto now = G()->unix_time();
  if (d->notification_settings.mute_until > now) {
    LOG(ERROR) << "Failed to unmute " << dialog_id << " in " << now << ", will be unmuted in "
               << d->notification_settings.mute_until;
    update_dialog_unmute_timeout(d, -1, d->notification_settings.mute_until);
    return;
  }

  LOG(INFO) << "Unmute " << dialog_id;
  update_dialog_unmute_timeout(d, d->notification_settings.mute_until, 0);
  d->notification_settings.mute_until = 0;
  send_closure(G()->td(), &Td::send_update,
               make_tl_object<td_api::updateNotificationSettings>(
                   get_notification_settings_scope_object(NotificationSettingsScope(dialog_id.get())),
                   get_notification_settings_object(&d->notification_settings)));
  on_dialog_updated(dialog_id, "on_dialog_unmute");
}

void MessagesManager::on_update_notify_settings(
    NotificationSettingsScope scope, tl_object_ptr<telegram_api::PeerNotifySettings> &&peer_notify_settings) {
  const NotificationSettings notification_settings = get_notification_settings(std::move(peer_notify_settings));
  if (!notification_settings.is_synchronized) {
    return;
  }

  NotificationSettings *current_settings = get_notification_settings(scope, true);
  if (current_settings == nullptr) {
    return;
  }
  update_notification_settings(scope, current_settings, notification_settings);
}

bool MessagesManager::get_dialog_report_spam_state(DialogId dialog_id, Promise<Unit> &&promise) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(3, "Chat not found"));
    return false;
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    promise.set_error(Status::Error(3, "Can't access the chat"));
    return false;
  }

  if (d->know_can_report_spam) {
    promise.set_value(Unit());
    return d->can_report_spam;
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
      td_->create_handler<GetPeerSettingsQuery>(std::move(promise))->send(dialog_id);
      return false;
    case DialogType::SecretChat:
      promise.set_value(Unit());
      return false;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }
}

void MessagesManager::change_dialog_report_spam_state(DialogId dialog_id, bool is_spam_dialog,
                                                      Promise<Unit> &&promise) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(3, "Can't access the chat"));
  }

  if (!d->know_can_report_spam || !d->can_report_spam) {
    return promise.set_error(Status::Error(3, "Can't update chat report spam state"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
      return td_->create_handler<UpdatePeerSettingsQuery>(std::move(promise))->send(dialog_id, is_spam_dialog);
    case DialogType::SecretChat:
      if (is_spam_dialog) {
        return td_->create_handler<ReportEncryptedSpamQuery>(std::move(promise))->send(dialog_id);
      } else {
        d->can_report_spam = false;
        on_dialog_updated(dialog_id, "change_dialog_report_spam_state");
        promise.set_value(Unit());
        return;
      }
    case DialogType::None:
    default:
      UNREACHABLE();
      return;
  }
}

bool MessagesManager::can_report_dialog(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      return td_->contacts_manager_->is_user_bot(dialog_id.get_user_id());
    case DialogType::Chat:
      return false;
    case DialogType::Channel:
      return !td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id()).is_creator();
    case DialogType::SecretChat:
      return false;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }
}

void MessagesManager::report_dialog(DialogId dialog_id, const tl_object_ptr<td_api::ChatReportReason> &reason,
                                    const vector<MessageId> &message_ids, Promise<Unit> &&promise) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(3, "Can't access the chat"));
  }

  if (reason == nullptr) {
    return promise.set_error(Status::Error(3, "Reason shouldn't be empty"));
  }

  if (!can_report_dialog(dialog_id)) {
    return promise.set_error(Status::Error(3, "Chat can't be reported"));
  }

  vector<MessageId> server_message_ids;
  for (auto message_id : message_ids) {
    if (message_id.is_valid() && message_id.is_server()) {
      server_message_ids.push_back(message_id);
    }
  }

  tl_object_ptr<telegram_api::ReportReason> report_reason;
  switch (reason->get_id()) {
    case td_api::chatReportReasonSpam::ID:
      report_reason = make_tl_object<telegram_api::inputReportReasonSpam>();
      break;
    case td_api::chatReportReasonViolence::ID:
      report_reason = make_tl_object<telegram_api::inputReportReasonViolence>();
      break;
    case td_api::chatReportReasonPornography::ID:
      report_reason = make_tl_object<telegram_api::inputReportReasonPornography>();
      break;
    case td_api::chatReportReasonCustom::ID: {
      auto other_reason = static_cast<const td_api::chatReportReasonCustom *>(reason.get());
      auto text = other_reason->text_;
      if (!clean_input_string(text)) {
        return promise.set_error(Status::Error(400, "Text must be encoded in UTF-8"));
      }

      report_reason = make_tl_object<telegram_api::inputReportReasonOther>(text);
      break;
    }
    default:
      UNREACHABLE();
  }
  CHECK(report_reason != nullptr);

  td_->create_handler<ReportPeerQuery>(std::move(promise))
      ->send(dialog_id, std::move(report_reason), server_message_ids);
}

void MessagesManager::on_get_peer_settings(DialogId dialog_id,
                                           tl_object_ptr<telegram_api::peerSettings> &&peer_settings) {
  CHECK(peer_settings != nullptr);
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return;
  }

  d->know_can_report_spam = true;
  d->can_report_spam = (peer_settings->flags_ & telegram_api::peerSettings::REPORT_SPAM_MASK) != 0;
  on_dialog_updated(dialog_id, "can_report_spam");
}

void MessagesManager::load_secret_thumbnail(FileId thumbnail_file_id) {
  class Callback : public FileManager::DownloadCallback {
   public:
    explicit Callback(Promise<> download_promise) : download_promise_(std::move(download_promise)) {
    }

    void on_download_ok(FileId file_id) override {
      download_promise_.set_value(Unit());
    }
    void on_download_error(FileId file_id, Status error) override {
      download_promise_.set_error(std::move(error));
    }

   private:
    Promise<> download_promise_;
  };

  auto thumbnail_promise = PromiseCreator::lambda([actor_id = actor_id(this),
                                                   thumbnail_file_id](Result<BufferSlice> r_thumbnail) mutable {
    BufferSlice thumbnail_slice;
    if (r_thumbnail.is_ok()) {
      thumbnail_slice = r_thumbnail.move_as_ok();
    }
    send_closure(actor_id, &MessagesManager::on_load_secret_thumbnail, thumbnail_file_id, std::move(thumbnail_slice));
  });

  auto download_promise = PromiseCreator::lambda(
      [thumbnail_file_id, thumbnail_promise = std::move(thumbnail_promise)](Result<Unit> r_download) mutable {
        if (r_download.is_error()) {
          thumbnail_promise.set_error(r_download.move_as_error());
          return;
        }
        send_closure(G()->file_manager(), &FileManager::get_content, thumbnail_file_id, std::move(thumbnail_promise));
      });

  send_closure(G()->file_manager(), &FileManager::download, thumbnail_file_id,
               std::make_unique<Callback>(std::move(download_promise)), 1);
}

void MessagesManager::on_upload_media(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file,
                                      tl_object_ptr<telegram_api::InputEncryptedFile> input_encrypted_file) {
  LOG(INFO) << "File " << file_id << " has been uploaded";

  auto it = being_uploaded_files_.find(file_id);
  if (it == being_uploaded_files_.end()) {
    // callback may be called just before the file upload was cancelled
    return;
  }

  auto full_message_id = it->second.first;
  auto thumbnail_file_id = it->second.second;

  being_uploaded_files_.erase(it);

  Message *m = get_message(full_message_id);
  if (m == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel, do not need to send it
    // file upload should be already cancelled in cancel_send_message_query, it shouldn't happen
    LOG(ERROR) << "Message with a media has already been deleted";
    return;
  }

  auto dialog_id = full_message_id.get_dialog_id();
  auto can_send_status = can_send_message(dialog_id);
  if (can_send_status.is_error()) {
    // user has left the chat during upload of the file or lost his privileges
    LOG(INFO) << "Can't send a message to " << dialog_id << ": " << can_send_status.error();

    int64 random_id = begin_send_message(dialog_id, m);
    on_send_message_fail(random_id, can_send_status.move_as_error());
    return;
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
      if (input_file && thumbnail_file_id.is_valid()) {
        // TODO: download thumbnail if needed (like in secret chats)
        being_uploaded_thumbnails_[thumbnail_file_id] = {full_message_id, file_id, std::move(input_file)};
        LOG(INFO) << "Ask to upload thumbnail " << thumbnail_file_id;
        td_->file_manager_->upload(thumbnail_file_id, upload_thumbnail_callback_, 1, m->message_id.get());
      } else {
        do_send_media(dialog_id, m, file_id, thumbnail_file_id, std::move(input_file), nullptr);
      }
      break;
    case DialogType::SecretChat:
      if (thumbnail_file_id.is_valid()) {
        being_loaded_secret_thumbnails_[thumbnail_file_id] = {full_message_id, file_id,
                                                              std::move(input_encrypted_file)};
        LOG(INFO) << "Ask to load thumbnail " << thumbnail_file_id;

        load_secret_thumbnail(thumbnail_file_id);
      } else {
        do_send_secret_media(dialog_id, m, file_id, thumbnail_file_id, std::move(input_encrypted_file), BufferSlice());
      }
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
      break;
  }
}

void MessagesManager::do_send_media(DialogId dialog_id, Message *m, FileId file_id, FileId thumbnail_file_id,
                                    tl_object_ptr<telegram_api::InputFile> input_file,
                                    tl_object_ptr<telegram_api::InputFile> input_thumbnail) {
  if (input_file == nullptr) {
    CHECK(input_thumbnail == nullptr);
    file_id = FileId();
    thumbnail_file_id = FileId();
  }
  CHECK(m != nullptr);
  on_message_media_uploaded(
      dialog_id, m, get_input_media(m->content.get(), std::move(input_file), std::move(input_thumbnail), m->ttl),
      file_id, thumbnail_file_id);
}

void MessagesManager::do_send_secret_media(DialogId dialog_id, Message *m, FileId file_id, FileId thumbnail_file_id,
                                           tl_object_ptr<telegram_api::InputEncryptedFile> input_encrypted_file,
                                           BufferSlice thumbnail) {
  if (input_encrypted_file == nullptr) {
    file_id = FileId();
    thumbnail_file_id = FileId();
  }

  CHECK(dialog_id.get_type() == DialogType::SecretChat);
  CHECK(m != nullptr);
  auto layer = td_->contacts_manager_->get_secret_chat_layer(dialog_id.get_secret_chat_id());
  on_secret_message_media_uploaded(
      dialog_id, m,
      get_secret_input_media(m->content.get(), std::move(input_encrypted_file), std::move(thumbnail), layer), file_id,
      thumbnail_file_id);
}

void MessagesManager::on_upload_media_error(FileId file_id, Status status) {
  if (G()->close_flag()) {
    // do not fail upload if closing
    return;
  }

  LOG(WARNING) << "File " << file_id << " has upload error " << status;
  CHECK(status.is_error());

  auto it = being_uploaded_files_.find(file_id);
  if (it == being_uploaded_files_.end()) {
    // callback may be called just before the file upload was cancelled
    return;
  }

  auto full_message_id = it->second.first;

  being_uploaded_files_.erase(it);

  fail_send_message(full_message_id, status.code() > 0 ? status.code() : 500,
                    status.message().str());  // TODO CHECK that status has always a code
}

void MessagesManager::on_load_secret_thumbnail(FileId thumbnail_file_id, BufferSlice thumbnail) {
  if (G()->close_flag()) {
    // do not send secret media if closing, thumbnail may be wrong
    return;
  }

  LOG(INFO) << "SecretThumbnail " << thumbnail_file_id << " has been loaded with size " << thumbnail.size();

  auto it = being_loaded_secret_thumbnails_.find(thumbnail_file_id);
  if (it == being_loaded_secret_thumbnails_.end()) {
    // just in case, as in on_upload_thumbnail
    return;
  }

  auto full_message_id = it->second.full_message_id;
  auto file_id = it->second.file_id;
  auto input_file = std::move(it->second.input_file);

  being_loaded_secret_thumbnails_.erase(it);

  Message *m = get_message(full_message_id);
  if (m == nullptr) {
    // message has already been deleted by the user, do not need to send it
    // cancel file upload of the main file to allow next upload with the same file to succeed
    td_->file_manager_->upload(file_id, nullptr, 0, 0);
    LOG(INFO) << "Message with a media has already been deleted";
    return;
  }

  if (thumbnail.empty()) {
    delete_message_content_thumbnail(m->content.get());
  }

  auto dialog_id = full_message_id.get_dialog_id();
  auto can_send_status = can_send_message(dialog_id);
  if (can_send_status.is_error()) {
    // secret chat was closed during load of the file
    LOG(INFO) << "Can't send a message to " << dialog_id << ": " << can_send_status.error();

    int64 random_id = begin_send_message(dialog_id, m);
    on_send_message_fail(random_id, can_send_status.move_as_error());
    return;
  }

  do_send_secret_media(dialog_id, m, file_id, thumbnail_file_id, std::move(input_file), std::move(thumbnail));
}

void MessagesManager::on_upload_thumbnail(FileId thumbnail_file_id,
                                          tl_object_ptr<telegram_api::InputFile> thumbnail_input_file) {
  if (G()->close_flag()) {
    // do not fail upload if closing
    return;
  }

  LOG(INFO) << "Thumbnail " << thumbnail_file_id << " has been uploaded as " << to_string(thumbnail_input_file);

  auto it = being_uploaded_thumbnails_.find(thumbnail_file_id);
  if (it == being_uploaded_thumbnails_.end()) {
    // callback may be called just before the thumbnail upload was cancelled
    return;
  }

  auto full_message_id = it->second.full_message_id;
  auto file_id = it->second.file_id;
  auto input_file = std::move(it->second.input_file);

  being_uploaded_thumbnails_.erase(it);

  Message *m = get_message(full_message_id);
  if (m == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel, do not need to send it
    // thumbnail file upload should be already cancelled in cancel_send_message_query
    LOG(ERROR) << "Message with a media has already been deleted";
    return;
  }

  if (thumbnail_input_file == nullptr) {
    delete_message_content_thumbnail(m->content.get());
  }

  auto dialog_id = full_message_id.get_dialog_id();
  auto can_send_status = can_send_message(dialog_id);
  if (can_send_status.is_error()) {
    // user has left the chat during upload of the thumbnail or lost his privileges
    LOG(INFO) << "Can't send a message to " << dialog_id << ": " << can_send_status.error();

    int64 random_id = begin_send_message(dialog_id, m);
    on_send_message_fail(random_id, can_send_status.move_as_error());
    return;
  }

  do_send_media(dialog_id, m, file_id, thumbnail_file_id, std::move(input_file), std::move(thumbnail_input_file));
}

void MessagesManager::on_upload_dialog_photo(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file) {
  LOG(INFO) << "File " << file_id << " has been uploaded";

  auto it = uploaded_dialog_photos_.find(file_id);
  if (it == uploaded_dialog_photos_.end()) {
    // just in case, as in on_upload_media
    return;
  }

  Promise<Unit> promise = std::move(it->second.promise);
  DialogId dialog_id = it->second.dialog_id;

  uploaded_dialog_photos_.erase(it);

  tl_object_ptr<telegram_api::InputChatPhoto> input_chat_photo;
  FileView file_view = td_->file_manager_->get_file_view(file_id);
  CHECK(!file_view.is_encrypted());
  if (file_view.has_remote_location()) {
    if (file_view.remote_location().is_web()) {
      // TODO reupload
      promise.set_error(Status::Error(400, "Can't use web photo as profile photo"));
      return;
    }
    input_chat_photo = make_tl_object<telegram_api::inputChatPhoto>(file_view.remote_location().as_input_photo());
    file_id = FileId();
  } else {
    CHECK(input_file != nullptr);
    input_chat_photo = make_tl_object<telegram_api::inputChatUploadedPhoto>(std::move(input_file));
  }

  // TODO invoke after
  td_->create_handler<EditDialogPhotoQuery>(std::move(promise))->send(file_id, dialog_id, std::move(input_chat_photo));
}

void MessagesManager::on_upload_dialog_photo_error(FileId file_id, Status status) {
  if (G()->close_flag()) {
    // do not fail upload if closing
    return;
  }

  LOG(INFO) << "File " << file_id << " has upload error " << status;
  CHECK(status.is_error());

  auto it = uploaded_dialog_photos_.find(file_id);
  if (it == uploaded_dialog_photos_.end()) {
    // just in case, as in on_upload_media_error
    return;
  }

  Promise<Unit> promise = std::move(it->second.promise);

  uploaded_dialog_photos_.erase(it);

  promise.set_error(std::move(status));
}

void MessagesManager::before_get_difference() {
  running_get_difference_ = true;

  postponed_pts_updates_.insert(std::make_move_iterator(pending_updates_.begin()),
                                std::make_move_iterator(pending_updates_.end()));

  drop_pending_updates();
}

void MessagesManager::after_get_difference() {
  CHECK(!td_->updates_manager_->running_get_difference());

  if (postponed_pts_updates_.size()) {
    LOG(INFO) << "Begin to apply postponed pts updates";
    auto old_pts = td_->updates_manager_->get_pts();
    for (auto &update : postponed_pts_updates_) {
      auto new_pts = update.second.pts;
      if (new_pts <= old_pts) {
        skip_old_pending_update(std::move(update.second.update), new_pts, old_pts, update.second.pts_count,
                                "after get difference");
      } else {
        add_pending_update(std::move(update.second.update), update.second.pts, update.second.pts_count, false,
                           "after get difference");
      }
      CHECK(!td_->updates_manager_->running_get_difference());
    }
    postponed_pts_updates_.clear();
    LOG(INFO) << "Finish to apply postponed pts updates";
  }

  running_get_difference_ = false;

  if (!pending_on_get_dialogs_.empty()) {
    LOG(INFO) << "Apply postponed results of getDialogs";
    for (auto &res : pending_on_get_dialogs_) {
      on_get_dialogs(std::move(res.dialogs), res.total_count, std::move(res.messages), std::move(res.promise));
    }
    pending_on_get_dialogs_.clear();
  }

  if (!postponed_chat_read_inbox_updates_.empty()) {
    LOG(INFO) << "Send postponed chat read inbox updates";
    auto dialog_ids = std::move(postponed_chat_read_inbox_updates_);
    for (auto dialog_id : dialog_ids) {
      send_update_chat_read_inbox(get_dialog(dialog_id), false, "after_get_difference");
    }
  }
  if (have_postponed_unread_message_count_update_) {
    LOG(INFO) << "Send postponed unread message count update";
    send_update_unread_message_count(DialogId(), false, "after_get_difference");
  }

  for (auto &it : update_message_ids_) {
    // this is impossible for ordinary chats because updates coming during getDifference have already been applied
    auto dialog_id = it.first.get_dialog_id();
    switch (dialog_id.get_type()) {
      case DialogType::Channel:
        // get channel difference may prevent updates from being applied
        if (running_get_channel_difference(dialog_id)) {
          break;
        }
      // fallthrough
      case DialogType::User:
      case DialogType::Chat:
        LOG(ERROR) << "Receive updateMessageId from " << it.second << " to " << it.first
                   << " but not receive corresponding message. " << td_->updates_manager_->get_state();
        if (dialog_id.get_type() != DialogType::Channel) {
          dump_debug_message_op(get_dialog(dialog_id));
        }
        break;
      case DialogType::SecretChat:
        break;
      case DialogType::None:
      default:
        UNREACHABLE();
        break;
    }
  }

  if (td_->is_online()) {
    // TODO move to AnimationsManager
    td_->animations_manager_->get_saved_animations(Auto());

    // TODO move to StickersManager
    td_->stickers_manager_->get_installed_sticker_sets(false, Auto());
    td_->stickers_manager_->get_installed_sticker_sets(true, Auto());
    td_->stickers_manager_->get_featured_sticker_sets(Auto());
    td_->stickers_manager_->get_recent_stickers(false, Auto());
    td_->stickers_manager_->get_recent_stickers(true, Auto());
    td_->stickers_manager_->get_favorite_stickers(Auto());
  }

  load_notification_settings();

  // TODO move to ContactsManager or delete after users will become persistent
  td_->contacts_manager_->get_user(td_->contacts_manager_->get_my_id("after_get_difference"), false, Promise<Unit>());

  // TODO resend some messages
}

void MessagesManager::on_get_messages(vector<tl_object_ptr<telegram_api::Message>> &&messages, bool is_channel_message,
                                      const char *source) {
  LOG(DEBUG) << "Receive " << messages.size() << " messages";
  for (auto &message : messages) {
    on_get_message(std::move(message), false, is_channel_message, false, false, source);
  }
}

void MessagesManager::on_get_history(DialogId dialog_id, MessageId from_message_id, int32 offset, int32 limit,
                                     bool from_the_end, vector<tl_object_ptr<telegram_api::Message>> &&messages) {
  LOG(INFO) << "Receive " << messages.size() << " history messages " << (from_the_end ? "from the end " : "") << "in "
            << dialog_id << " from " << from_message_id << " with offset " << offset << " and limit " << limit;
  CHECK(-limit < offset && offset <= 0);
  CHECK(offset < 0 || from_the_end);

  if (narrow_cast<int32>(messages.size()) < limit + offset && !messages.empty()) {
    MessageId first_received_message_id = get_message_id(messages.back());
    if (first_received_message_id.get() >= from_message_id.get()) {
      // it is likely that there is no more history messages on the server
      Dialog *d = get_dialog(dialog_id);
      if (d != nullptr && d->first_database_message_id.is_valid() &&
          d->first_database_message_id.get() <= first_received_message_id.get()) {
        d->have_full_history = true;
        on_dialog_updated(dialog_id, "set have_full_history");
      }
    }
  }

  if (from_the_end && narrow_cast<int32>(messages.size()) < limit) {
    // it is likely that there is no more history messages on the server
    Dialog *d = get_dialog(dialog_id);
    if (d != nullptr) {
      d->have_full_history = true;
      on_dialog_updated(dialog_id, "set have_full_history");
    }
  }

  if (messages.empty()) {
    if (from_the_end) {
      Dialog *d = get_dialog(dialog_id);
      if (d != nullptr && d->have_full_history) {
        set_dialog_is_empty(d, "on_get_history empty");
      }
    }

    // be aware that in some cases an empty answer may be returned, because of the race of getHistory and deleteMessages
    // and not because there is no more messages
    return;
  }

  // TODO check that messages are received in decreasing message_id order

  // be aware that dialog may not yet exist
  // be aware that returned messages are guaranteed to be consecutive messages, but if !from_the_end they
  // may be older (if some messages was deleted) or newer (if some messages were added) than an expected answer
  // be aware that any subset of the returned messages may be already deleted and returned as MessageEmpty
  bool is_channel_message = dialog_id.get_type() == DialogType::Channel;
  MessageId first_added_message_id;
  MessageId last_received_message_id = get_message_id(messages[0]);
  MessageId last_added_message_id;
  bool have_next = false;
  Dialog *d = get_dialog(dialog_id);

  MessageId prev_last_new_message_id;
  MessageId prev_first_database_message_id;
  MessageId prev_last_database_message_id;
  MessageId prev_last_message_id;
  if (d != nullptr) {
    prev_last_new_message_id = d->last_new_message_id;
    prev_first_database_message_id = d->first_database_message_id;
    prev_last_database_message_id = d->last_database_message_id;
    prev_last_message_id = d->last_message_id;
  }

  for (auto &message : messages) {
    if (!have_next && from_the_end && get_message_id(message).get() < d->last_message_id.get()) {
      // last message in the dialog should be attached to the next message if there is some
      have_next = true;
    }

    auto message_dialog_id = get_message_dialog_id(message);
    if (message_dialog_id != dialog_id) {
      LOG(ERROR) << "Receive " << get_message_id(message) << " in wrong " << message_dialog_id << " instead of "
                 << dialog_id << ": " << oneline(to_string(message));
      continue;
    }

    auto full_message_id =
        on_get_message(std::move(message), false, is_channel_message, false, have_next, "get history");
    auto message_id = full_message_id.get_message_id();
    if (message_id.is_valid()) {
      if (!last_added_message_id.is_valid()) {
        last_added_message_id = message_id;
      }

      if (!have_next) {
        if (d == nullptr) {
          d = get_dialog(dialog_id);
          CHECK(d != nullptr);
        }
        have_next = true;
      } else if (first_added_message_id.is_valid()) {
        Message *next_message = get_message(d, first_added_message_id);
        CHECK(next_message != nullptr);
        if (!next_message->have_previous) {
          LOG(INFO) << "Fix have_previous for " << first_added_message_id;
          next_message->have_previous = true;
          attach_message_to_previous(d, first_added_message_id);
        }
      }
      first_added_message_id = message_id;
      if (!message_id.is_yet_unsent()) {
        // message should be already saved to database in on_get_message
        // add_message_to_database(d, get_message(d, message_id), "on_get_history");
      }
    }
  }

  if (d == nullptr) {
    return;
  }

  //  LOG_IF(ERROR, d->first_message_id.is_valid() && d->first_message_id.get() > first_received_message_id.get())
  //      << "Receive message " << first_received_message_id << ", but first chat message is " << d->first_message_id;

  bool need_update_database_message_ids =
      last_added_message_id.is_valid() &&
      (from_the_end || (last_added_message_id.get() >= d->first_database_message_id.get() &&
                        d->last_database_message_id.get() >= first_added_message_id.get()));
  if (from_the_end) {
    if (!d->last_new_message_id.is_valid()) {
      set_dialog_last_new_message_id(
          d, last_added_message_id.is_valid() ? last_added_message_id : last_received_message_id, "on_get_history");
    }
    if (last_added_message_id.is_valid()) {
      if (last_added_message_id.get() > d->last_message_id.get()) {
        CHECK(d->last_new_message_id.is_valid());
        set_dialog_last_message_id(d, last_added_message_id, "on_get_history");
        send_update_chat_last_message(d, "on_get_history");
      }
    }
  }

  if (need_update_database_message_ids) {
    bool is_dialog_updated = false;
    if (!d->last_database_message_id.is_valid()) {
      CHECK(d->last_message_id.is_valid());
      MessagesConstIterator it(d, d->last_message_id);
      while (*it != nullptr) {
        auto message_id = (*it)->message_id;
        if (message_id.is_server() || message_id.is_local()) {
          if (!d->last_database_message_id.is_valid()) {
            set_dialog_last_database_message_id(d, message_id, "on_get_history");
          }
          set_dialog_first_database_message_id(d, message_id, "on_get_history");
          try_restore_dialog_reply_markup(d, *it);
        }
        --it;
      }
      is_dialog_updated = true;
    } else {
      CHECK(d->last_new_message_id.is_valid())
          << dialog_id << " " << from_the_end << " " << d->first_database_message_id << " "
          << d->last_database_message_id << " " << first_added_message_id << " " << last_added_message_id << " "
          << d->last_message_id << prev_last_new_message_id << prev_first_database_message_id
          << prev_last_database_message_id << prev_last_message_id;
      CHECK(d->first_database_message_id.is_valid());
      {
        MessagesConstIterator it(d, d->first_database_message_id);
        if (*it != nullptr && ((*it)->message_id == d->first_database_message_id || (*it)->have_next)) {
          while (*it != nullptr) {
            auto message_id = (*it)->message_id;
            if ((message_id.is_server() || message_id.is_local()) &&
                message_id.get() < d->first_database_message_id.get()) {
              set_dialog_first_database_message_id(d, message_id, "on_get_history 2");
              try_restore_dialog_reply_markup(d, *it);
              is_dialog_updated = true;
            }
            --it;
          }
        }
      }
      {
        MessagesConstIterator it(d, d->last_database_message_id);
        if (*it != nullptr && ((*it)->message_id == d->last_database_message_id || (*it)->have_next)) {
          while (*it != nullptr) {
            auto message_id = (*it)->message_id;
            if ((message_id.is_server() || message_id.is_local()) &&
                message_id.get() > d->last_database_message_id.get()) {
              set_dialog_last_database_message_id(d, message_id, "on_get_history 2");
              is_dialog_updated = true;
            }
            ++it;
          }
        }
      }
    }
    CHECK(d->first_database_message_id.is_valid());
    CHECK(d->last_database_message_id.is_valid());

    for (auto &first_message_id : d->first_database_message_id_by_index) {
      if (first_added_message_id.get() < first_message_id.get() &&
          first_message_id.get() <= last_added_message_id.get()) {
        first_message_id = first_added_message_id;
      }
    }

    if (is_dialog_updated) {
      on_dialog_updated(dialog_id, "on_get_history");
    }
  }
}

vector<DialogId> MessagesManager::get_peers_dialog_ids(vector<tl_object_ptr<telegram_api::Peer>> &&peers) {
  vector<DialogId> result;
  result.reserve(peers.size());
  for (auto &peer : peers) {
    DialogId dialog_id(peer);
    if (dialog_id.is_valid()) {
      force_create_dialog(dialog_id, "get_peers_dialog_ids");
      result.push_back(dialog_id);
    }
  }
  return result;
}

void MessagesManager::on_get_public_dialogs_search_result(const string &query,
                                                          vector<tl_object_ptr<telegram_api::Peer>> &&my_peers,
                                                          vector<tl_object_ptr<telegram_api::Peer>> &&peers) {
  auto it = search_public_dialogs_queries_.find(query);
  CHECK(it != search_public_dialogs_queries_.end());
  CHECK(it->second.size() > 0);
  auto promises = std::move(it->second);
  search_public_dialogs_queries_.erase(it);

  found_public_dialogs_[query] = get_peers_dialog_ids(std::move(peers));
  found_on_server_dialogs_[query] = get_peers_dialog_ids(std::move(my_peers));

  for (auto &promise : promises) {
    promise.set_value(Unit());
  }
}

void MessagesManager::on_failed_public_dialogs_search(const string &query, Status &&error) {
  auto it = search_public_dialogs_queries_.find(query);
  CHECK(it != search_public_dialogs_queries_.end());
  CHECK(it->second.size() > 0);
  auto promises = std::move(it->second);
  search_public_dialogs_queries_.erase(it);

  found_public_dialogs_[query];     // negative cache
  found_on_server_dialogs_[query];  // negative cache

  for (auto &promise : promises) {
    promise.set_error(error.clone());
  }
}

void MessagesManager::on_get_dialog_messages_search_result(DialogId dialog_id, const string &query,
                                                           UserId sender_user_id, MessageId from_message_id,
                                                           int32 offset, int32 limit, SearchMessagesFilter filter,
                                                           int64 random_id, int32 total_count,
                                                           vector<tl_object_ptr<telegram_api::Message>> &&messages) {
  LOG(INFO) << "Receive " << messages.size() << " found messages in " << dialog_id;
  if (!dialog_id.is_valid()) {
    CHECK(query.empty());
    CHECK(!sender_user_id.is_valid());
    auto it = found_call_messages_.find(random_id);
    CHECK(it != found_call_messages_.end());

    MessageId first_added_message_id;
    if (messages.empty()) {
      // messages may be empty because there is no more messages or they can't be found due to global limit
      // anyway pretend that there is no more messages
      first_added_message_id = MessageId::min();
    }

    auto &result = it->second.second;
    CHECK(result.empty());
    for (auto &message : messages) {
      auto new_message = on_get_message(std::move(message), false, false, false, false, "search call messages");
      if (new_message != FullMessageId()) {
        result.push_back(new_message);
      }

      auto message_id = new_message.get_message_id();
      if (message_id.get() < first_added_message_id.get() || !first_added_message_id.is_valid()) {
        first_added_message_id = message_id;
      }
    }
    if (G()->parameters().use_message_db) {
      bool update_state = false;

      auto &old_message_count = calls_db_state_.message_count_by_index[search_calls_filter_index(filter)];
      if (old_message_count != total_count) {
        LOG(INFO) << "Update calls database message count to " << total_count;
        old_message_count = total_count;
        update_state = true;
      }

      auto &old_first_db_message_id =
          calls_db_state_.first_calls_database_message_id_by_index[search_calls_filter_index(filter)];
      bool from_the_end = !from_message_id.is_valid() || from_message_id.get() >= MessageId::max().get();
      LOG(INFO) << "from_the_end = " << from_the_end << ", old_first_db_message_id = " << old_first_db_message_id.get()
                << ", first_added_message_id = " << first_added_message_id.get()
                << ", from_message_id = " << from_message_id.get();
      if ((from_the_end ||
           (old_first_db_message_id.is_valid() && old_first_db_message_id.get() <= from_message_id.get())) &&
          (!old_first_db_message_id.is_valid() || first_added_message_id.get() < old_first_db_message_id.get())) {
        LOG(INFO) << "Update calls database first message id to " << first_added_message_id;
        old_first_db_message_id = first_added_message_id;
        update_state = true;
      }
      if (update_state) {
        save_calls_db_state();
      }
    }
    it->second.first = total_count;
    return;
  }

  auto it = found_dialog_messages_.find(random_id);
  CHECK(it != found_dialog_messages_.end());

  auto &result = it->second.second;
  CHECK(result.empty());
  MessageId first_added_message_id;
  if (messages.empty()) {
    // messages may be empty because there is no more messages or they can't be found due to global limit
    // anyway pretend that there is no more messages
    first_added_message_id = MessageId::min();
  }
  for (auto &message : messages) {
    auto new_message = on_get_message(std::move(message), false, dialog_id.get_type() == DialogType::Channel, false,
                                      false, "search chat messages");
    if (new_message != FullMessageId()) {
      if (new_message.get_dialog_id() != dialog_id) {
        LOG(ERROR) << "Receive " << new_message << " instead of a message in " << dialog_id;
        continue;
      }

      // TODO check that messages are returned in decreasing message_id order
      auto message_id = new_message.get_message_id();
      if (message_id.get() < first_added_message_id.get() || !first_added_message_id.is_valid()) {
        first_added_message_id = message_id;
      }
      result.push_back(message_id);
    } else {
      total_count--;
    }
  }
  if (total_count < static_cast<int32>(result.size())) {
    LOG(ERROR) << "Receive " << result.size() << " valid messages out of " << total_count << " in " << messages.size()
               << " messages";
    total_count = static_cast<int32>(result.size());
  }
  if (query.empty() && !sender_user_id.is_valid() && filter != SearchMessagesFilter::Empty &&
      G()->parameters().use_message_db) {
    Dialog *d = get_dialog(dialog_id);
    CHECK(d != nullptr);
    bool update_dialog = false;

    auto &old_message_count = d->message_count_by_index[search_messages_filter_index(filter)];
    if (old_message_count != total_count) {
      old_message_count = total_count;
      if (filter == SearchMessagesFilter::UnreadMention) {
        d->unread_mention_count = old_message_count;
        send_update_chat_unread_mention_count(d);
      }
      update_dialog = true;
    }

    auto &old_first_db_message_id = d->first_database_message_id_by_index[search_messages_filter_index(filter)];
    bool from_the_end = !from_message_id.is_valid() ||
                        (d->last_message_id != MessageId() && from_message_id.get() > d->last_message_id.get()) ||
                        from_message_id.get() >= MessageId::max().get();
    if ((from_the_end ||
         (old_first_db_message_id.is_valid() && old_first_db_message_id.get() <= from_message_id.get())) &&
        (!old_first_db_message_id.is_valid() || first_added_message_id.get() < old_first_db_message_id.get())) {
      old_first_db_message_id = first_added_message_id;
      update_dialog = true;
    }
    if (update_dialog) {
      on_dialog_updated(dialog_id, "search results");
    }
  }
  it->second.first = total_count;
}

void MessagesManager::on_failed_dialog_messages_search(DialogId dialog_id, int64 random_id) {
  if (!dialog_id.is_valid()) {
    auto it = found_call_messages_.find(random_id);
    CHECK(it != found_call_messages_.end());
    found_call_messages_.erase(it);
    return;
  }

  auto it = found_dialog_messages_.find(random_id);
  CHECK(it != found_dialog_messages_.end());
  found_dialog_messages_.erase(it);
}

void MessagesManager::on_get_messages_search_result(const string &query, int32 offset_date, DialogId offset_dialog_id,
                                                    MessageId offset_message_id, int32 limit, int64 random_id,
                                                    int32 total_count,
                                                    vector<tl_object_ptr<telegram_api::Message>> &&messages) {
  LOG(INFO) << "Receive " << messages.size() << " found messages";
  auto it = found_messages_.find(random_id);
  CHECK(it != found_messages_.end());

  auto &result = it->second.second;
  CHECK(result.empty());
  for (auto &message : messages) {
    auto dialog_id = get_message_dialog_id(message);
    auto new_message = on_get_message(std::move(message), false, dialog_id.get_type() == DialogType::Channel, false,
                                      false, "search messages");
    if (new_message != FullMessageId()) {
      CHECK(dialog_id == new_message.get_dialog_id());
      result.push_back(new_message);
    } else {
      total_count--;
    }
  }
  if (total_count < static_cast<int32>(result.size())) {
    LOG(ERROR) << "Receive " << result.size() << " valid messages out of " << total_count << " in " << messages.size()
               << " messages";
    total_count = static_cast<int32>(result.size());
  }
  it->second.first = total_count;
}

void MessagesManager::on_failed_messages_search(int64 random_id) {
  auto it = found_messages_.find(random_id);
  CHECK(it != found_messages_.end());
  found_messages_.erase(it);
}

void MessagesManager::on_get_recent_locations(DialogId dialog_id, int32 limit, int64 random_id, int32 total_count,
                                              vector<tl_object_ptr<telegram_api::Message>> &&messages) {
  LOG(INFO) << "Receive " << messages.size() << " recent locations in " << dialog_id;
  auto it = found_dialog_recent_location_messages_.find(random_id);
  CHECK(it != found_dialog_recent_location_messages_.end());

  auto &result = it->second.second;
  CHECK(result.empty());
  for (auto &message : messages) {
    auto new_message = on_get_message(std::move(message), false, dialog_id.get_type() == DialogType::Channel, false,
                                      false, "get recent locations");
    if (new_message != FullMessageId()) {
      if (new_message.get_dialog_id() != dialog_id) {
        LOG(ERROR) << "Receive " << new_message << " instead of a message in " << dialog_id;
        continue;
      }
      auto m = get_message(new_message);
      if (m->content->get_id() != MessageLiveLocation::ID) {
        LOG(ERROR) << "Receive a message of wrong type " << m->content->get_id() << " in on_get_recent_locations in "
                   << dialog_id;
        continue;
      }

      result.push_back(new_message.get_message_id());
    } else {
      total_count--;
    }
  }
  if (total_count < static_cast<int32>(result.size())) {
    LOG(ERROR) << "Receive " << result.size() << " valid messages out of " << total_count << " in " << messages.size()
               << " messages";
    total_count = static_cast<int32>(result.size());
  }
  it->second.first = total_count;
}

void MessagesManager::on_get_recent_locations_failed(int64 random_id) {
  auto it = found_dialog_recent_location_messages_.find(random_id);
  CHECK(it != found_dialog_recent_location_messages_.end());
  found_dialog_recent_location_messages_.erase(it);
}

void MessagesManager::delete_messages_from_updates(const vector<MessageId> &message_ids) {
  std::unordered_map<DialogId, vector<int64>, DialogIdHash> deleted_message_ids;
  std::unordered_map<DialogId, bool, DialogIdHash> need_update_dialog_pos;
  for (auto message_id : message_ids) {
    if (!message_id.is_valid() || !message_id.is_server()) {
      LOG(ERROR) << "Incoming update tries to delete " << message_id;
      continue;
    }

    Dialog *d = get_dialog_by_message_id(message_id);
    if (d != nullptr) {
      auto m = delete_message(d, message_id, true, &need_update_dialog_pos[d->dialog_id], "updates");
      CHECK(m != nullptr);
      CHECK(m->message_id == message_id);
      deleted_message_ids[d->dialog_id].push_back(message_id.get());
    }
    if (last_clear_history_message_id_to_dialog_id_.count(message_id)) {
      d = get_dialog(last_clear_history_message_id_to_dialog_id_[message_id]);
      CHECK(d != nullptr);
      auto m = delete_message(d, message_id, true, &need_update_dialog_pos[d->dialog_id], "updates");
      CHECK(m == nullptr);
    }
  }
  for (auto &it : need_update_dialog_pos) {
    if (it.second) {
      auto dialog_id = it.first;
      Dialog *d = get_dialog(dialog_id);
      CHECK(d != nullptr);
      send_update_chat_last_message(d, "delete_messages_from_updates");
    }
  }
  for (auto &it : deleted_message_ids) {
    auto dialog_id = it.first;
    send_update_delete_messages(dialog_id, std::move(it.second), true, false);
  }
}

void MessagesManager::delete_dialog_messages_from_updates(DialogId dialog_id, const vector<MessageId> &message_ids) {
  CHECK(dialog_id.get_type() == DialogType::Channel || dialog_id.get_type() == DialogType::SecretChat);
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(INFO) << "Ignore deleteChannelMessages for unknown " << dialog_id;
    CHECK(dialog_id.get_type() == DialogType::Channel);
    return;
  }

  vector<int64> deleted_message_ids;
  bool need_update_dialog_pos = false;
  for (auto message_id : message_ids) {
    if (!message_id.is_valid() || (!message_id.is_server() && dialog_id.get_type() != DialogType::SecretChat)) {
      LOG(ERROR) << "Incoming update tries to delete " << message_id;
      continue;
    }

    if (delete_message(d, message_id, true, &need_update_dialog_pos, "updates") != nullptr) {
      deleted_message_ids.push_back(message_id.get());
    }
  }
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "delete_dialog_messages_from_updates");
  }
  send_update_delete_messages(dialog_id, std::move(deleted_message_ids), true, false);
}

bool MessagesManager::is_secret_message_content(int32 ttl, int32 content_type) {
  if (ttl <= 0 || ttl > 60) {
    return false;
  }
  switch (content_type) {
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessagePhoto::ID:
    case MessageVideo::ID:
    case MessageVideoNote::ID:
    case MessageVoiceNote::ID:
      return true;
    case MessageContact::ID:
    case MessageDocument::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessageSticker::ID:
    case MessageText::ID:
    case MessageUnsupported::ID:
    case MessageVenue::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

bool MessagesManager::is_service_message_content(int32 content_type) {
  switch (content_type) {
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageContact::ID:
    case MessageDocument::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessagePhoto::ID:
    case MessageSticker::ID:
    case MessageText::ID:
    case MessageUnsupported::ID:
    case MessageVenue::ID:
    case MessageVideo::ID:
    case MessageVideoNote::ID:
    case MessageVoiceNote::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
      return false;
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return true;
    default:
      UNREACHABLE();
      return false;
  }
}

bool MessagesManager::can_have_message_content_caption(int32 content_type) {
  switch (content_type) {
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageDocument::ID:
    case MessagePhoto::ID:
    case MessageVideo::ID:
    case MessageVoiceNote::ID:
      return true;
    case MessageContact::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessageSticker::ID:
    case MessageText::ID:
    case MessageUnsupported::ID:
    case MessageVenue::ID:
    case MessageVideoNote::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

string MessagesManager::get_search_text(const Message *m) {
  if (m->is_content_secret) {
    return "";
  }
  switch (m->content->get_id()) {
    case MessageText::ID: {
      auto *text = static_cast<const MessageText *>(m->content.get());
      if (!text->web_page_id.is_valid()) {
        return text->text.text;
      }
      return PSTRING() << text->text.text << " "
                       << td_->web_pages_manager_->get_web_page_search_text(text->web_page_id);
    }
    case MessageAnimation::ID: {
      auto animation = static_cast<const MessageAnimation *>(m->content.get());
      return PSTRING() << td_->animations_manager_->get_animation_search_text(animation->file_id) << " "
                       << animation->caption.text;
    }
    case MessageAudio::ID: {
      auto audio = static_cast<const MessageAudio *>(m->content.get());
      return PSTRING() << td_->audios_manager_->get_audio_search_text(audio->file_id) << " " << audio->caption.text;
    }
    case MessageDocument::ID: {
      auto document = static_cast<const MessageDocument *>(m->content.get());
      return PSTRING() << td_->documents_manager_->get_document_search_text(document->file_id) << " "
                       << document->caption.text;
    }
    case MessagePhoto::ID: {
      auto photo = static_cast<const MessagePhoto *>(m->content.get());
      return PSTRING() << photo->caption.text;
    }
    case MessageVideo::ID: {
      auto video = static_cast<const MessageVideo *>(m->content.get());
      return PSTRING() << td_->videos_manager_->get_video_search_text(video->file_id) << " " << video->caption.text;
    }
    case MessageContact::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessageSticker::ID:
    case MessageUnsupported::ID:
    case MessageVenue::ID:
    case MessageVideoNote::ID:
    case MessageVoiceNote::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return "";
    default:
      UNREACHABLE();
      return "";
  }
}

bool MessagesManager::is_allowed_media_group_content(int32 content_type) {
  switch (content_type) {
    case MessagePhoto::ID:
    case MessageVideo::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
      return true;
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageContact::ID:
    case MessageDocument::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessageSticker::ID:
    case MessageText::ID:
    case MessageUnsupported::ID:
    case MessageVenue::ID:
    case MessageVideoNote::ID:
    case MessageVoiceNote::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

bool MessagesManager::can_forward_message(DialogId from_dialog_id, const Message *m) {
  if (m == nullptr) {
    return false;
  }
  if (m->ttl > 0) {
    return false;
  }
  switch (from_dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
      // ok
      break;
    case DialogType::SecretChat:
      return false;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }

  auto content_id = m->content->get_id();
  return !is_service_message_content(content_id) && content_id != MessageUnsupported::ID &&
         content_id != MessageExpiredPhoto::ID && content_id != MessageExpiredVideo::ID;
}

bool MessagesManager::can_delete_channel_message(DialogParticipantStatus status, const Message *m, bool is_bot) {
  if (m == nullptr) {
    return true;
  }
  if (m->message_id.is_local() || m->message_id.is_yet_unsent()) {
    return true;
  }

  if (is_bot && G()->unix_time_cached() >= m->date + 2 * 86400) {
    // bots can't delete messages older than 2 days
    return false;
  }

  CHECK(m->message_id.is_server());
  if (m->message_id.get_server_message_id().get() == 1) {
    return false;
  }
  auto content_id = m->content->get_id();
  if (content_id == MessageChannelMigrateFrom::ID || content_id == MessageChannelCreate::ID) {
    return false;
  }

  if (status.can_delete_messages()) {
    return true;
  }

  if (!m->is_outgoing) {
    return false;
  }

  if (m->is_channel_post || is_service_message_content(content_id)) {
    return status.can_post_messages();
  }

  return true;
}

bool MessagesManager::can_revoke_message(DialogId dialog_id, const Message *m) const {
  if (m == nullptr) {
    return false;
  }
  if (m->message_id.is_local()) {
    return false;
  }
  if (dialog_id == DialogId(td_->contacts_manager_->get_my_id("can_revoke_message"))) {
    return false;
  }
  if (m->message_id.is_yet_unsent()) {
    return true;
  }
  CHECK(m->message_id.is_server());

  bool is_appointed_administrator = false;
  bool can_revoke_incoming = false;
  const int32 DEFAULT_REVOKE_TIME_LIMIT = 2 * 86400;
  int32 revoke_time_limit = G()->shared_config().get_option_integer("revoke_time_limit", DEFAULT_REVOKE_TIME_LIMIT);
  switch (dialog_id.get_type()) {
    case DialogType::User:
      can_revoke_incoming = G()->shared_config().get_option_boolean("revoke_pm_inbox");
      revoke_time_limit = G()->shared_config().get_option_integer("revoke_pm_time_limit", DEFAULT_REVOKE_TIME_LIMIT);
      break;
    case DialogType::Chat:
      is_appointed_administrator = td_->contacts_manager_->is_appointed_chat_administrator(dialog_id.get_chat_id());
      break;
    case DialogType::Channel:
      return true;  // any server message that can be deleted will be deleted for all participants
    case DialogType::SecretChat:
      return !is_service_message_content(
          m->content->get_id());  // all non-service messages will be deleted for everyone
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }

  return (((m->is_outgoing || can_revoke_incoming) && !is_service_message_content(m->content->get_id())) ||
          is_appointed_administrator) &&
         G()->unix_time_cached() - m->date <= revoke_time_limit;
}

void MessagesManager::delete_messages(DialogId dialog_id, const vector<MessageId> &input_message_ids, bool revoke,
                                      Promise<Unit> &&promise) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(6, "Chat is not found"));
  }

  if (input_message_ids.empty()) {
    return promise.set_value(Unit());
  }

  auto dialog_type = dialog_id.get_type();
  bool is_secret = dialog_type == DialogType::SecretChat;

  vector<MessageId> message_ids;
  message_ids.reserve(input_message_ids.size());
  vector<MessageId> deleted_server_message_ids;
  for (auto message_id : input_message_ids) {
    if (!message_id.is_valid()) {
      return promise.set_error(Status::Error(6, "Invalid message identifier"));
    }
    message_id = get_persistent_message_id(d, message_id);
    message_ids.push_back(message_id);
    if (get_message_force(d, message_id) != nullptr && (message_id.is_server() || is_secret)) {
      deleted_server_message_ids.push_back(message_id);
    }
  }

  bool is_bot = td_->auth_manager_->is_bot();
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      if (is_bot) {
        for (auto message_id : message_ids) {
          if (message_id.is_server() && !can_revoke_message(dialog_id, get_message(d, message_id))) {
            return promise.set_error(Status::Error(6, "Message can't be deleted"));
          }
        }
      }
      break;
    case DialogType::Channel: {
      auto dialog_status = td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id());
      for (auto message_id : message_ids) {
        if (!can_delete_channel_message(dialog_status, get_message(d, message_id), is_bot)) {
          return promise.set_error(Status::Error(6, "Message can't be deleted"));
        }
      }
      break;
    }
    case DialogType::SecretChat:
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  delete_messages_from_server(dialog_id, std::move(deleted_server_message_ids), revoke, 0, std::move(promise));

  bool need_update_dialog_pos = false;
  vector<int64> deleted_message_ids;
  for (auto message_id : message_ids) {
    auto m = delete_message(d, message_id, true, &need_update_dialog_pos, DELETE_MESSAGE_USER_REQUEST_SOURCE);
    if (m == nullptr) {
      LOG(INFO) << "Can't delete " << message_id << " because it is not found";
    } else {
      deleted_message_ids.push_back(m->message_id.get());
    }
  }

  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "delete_messages");
  }
  send_update_delete_messages(dialog_id, std::move(deleted_message_ids), true, false);
}

class MessagesManager::DeleteMessagesFromServerLogEvent {
 public:
  DialogId dialog_id_;
  vector<MessageId> message_ids_;
  bool revoke_;

  template <class StorerT>
  void store(StorerT &storer) const {
    BEGIN_STORE_FLAGS();
    STORE_FLAG(revoke_);
    END_STORE_FLAGS();

    td::store(dialog_id_, storer);
    td::store(message_ids_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    BEGIN_PARSE_FLAGS();
    PARSE_FLAG(revoke_);
    END_PARSE_FLAGS();

    td::parse(dialog_id_, parser);
    td::parse(message_ids_, parser);
  }
};

void MessagesManager::delete_messages_from_server(DialogId dialog_id, vector<MessageId> message_ids, bool revoke,
                                                  uint64 logevent_id, Promise<Unit> &&promise) {
  if (message_ids.empty()) {
    promise.set_value(Unit());
    return;
  }
  LOG(INFO) << (revoke ? "Revoke " : "Delete ") << format::as_array(message_ids) << " in " << dialog_id
            << " from server";

  if (logevent_id == 0 && G()->parameters().use_message_db) {
    DeleteMessagesFromServerLogEvent logevent;
    logevent.dialog_id_ = dialog_id;
    logevent.message_ids_ = message_ids;
    logevent.revoke_ = revoke;

    auto storer = LogEventStorerImpl<DeleteMessagesFromServerLogEvent>(logevent);
    logevent_id =
        BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::DeleteMessagesFromServer, storer);
  }

  if (logevent_id != 0) {
    auto new_promise = PromiseCreator::lambda([logevent_id, promise = std::move(promise)](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }

      promise.set_result(std::move(result));
    });
    promise = std::move(new_promise);
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      td_->create_handler<DeleteMessagesQuery>(std::move(promise))->send(std::move(message_ids), revoke);
      break;
    case DialogType::Channel:
      td_->create_handler<DeleteChannelMessagesQuery>(std::move(promise))
          ->send(dialog_id.get_channel_id(), std::move(message_ids));
      break;
    case DialogType::SecretChat: {
      vector<int64> random_ids;
      auto d = get_dialog_force(dialog_id);
      CHECK(d != nullptr);
      for (auto &message_id : message_ids) {
        auto *message = get_message(d, message_id);
        if (message != nullptr) {
          random_ids.push_back(message->random_id);
        }
      }
      if (!random_ids.empty()) {
        send_closure(G()->secret_chats_manager(), &SecretChatsManager::delete_messages, dialog_id.get_secret_chat_id(),
                     std::move(random_ids), std::move(promise));
      } else {
        promise.set_value(Unit());
      }
      break;
    }
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void MessagesManager::delete_dialog_history(DialogId dialog_id, bool remove_from_dialog_list, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive deleteChatHistory request to delete all messages in " << dialog_id
            << ", remove_from_chat_list is " << remove_from_dialog_list;

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(3, "Chat info not found"));
  }

  auto dialog_type = dialog_id.get_type();
  bool is_secret = false;
  switch (dialog_type) {
    case DialogType::User:
    case DialogType::Chat:
      // ok
      break;
    case DialogType::Channel:
      if (is_broadcast_channel(dialog_id)) {
        return promise.set_error(Status::Error(3, "Can't delete chat history in a channel"));
      }
      if (!get_dialog_username(dialog_id).empty()) {
        return promise.set_error(Status::Error(3, "Can't delete chat history in a public supergroup"));
      }
      break;
    case DialogType::SecretChat:
      is_secret = true;
      // ok
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
      break;
  }

  auto last_new_message_id = d->last_new_message_id;
  if (!is_secret && last_new_message_id == MessageId()) {
    // TODO get dialog from the server and delete history from last message id
  }

  bool allow_error = d->messages == nullptr;

  delete_all_dialog_messages(d, remove_from_dialog_list, true);

  if (last_new_message_id.is_valid() && last_new_message_id == d->max_unavailable_message_id) {
    // history has already been cleared, nothing to do
    promise.set_value(Unit());
    return;
  }

  set_dialog_max_unavailable_message_id(dialog_id, last_new_message_id, false, "delete_dialog_history");

  delete_dialog_history_from_server(dialog_id, last_new_message_id, remove_from_dialog_list, allow_error, 0,
                                    std::move(promise));
}

class MessagesManager::DeleteDialogHistoryFromServerLogEvent {
 public:
  DialogId dialog_id_;
  MessageId max_message_id_;
  bool remove_from_dialog_list_;

  template <class StorerT>
  void store(StorerT &storer) const {
    BEGIN_STORE_FLAGS();
    STORE_FLAG(remove_from_dialog_list_);
    END_STORE_FLAGS();

    td::store(dialog_id_, storer);
    td::store(max_message_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    BEGIN_PARSE_FLAGS();
    PARSE_FLAG(remove_from_dialog_list_);
    END_PARSE_FLAGS();

    td::parse(dialog_id_, parser);
    td::parse(max_message_id_, parser);
  }
};

void MessagesManager::delete_dialog_history_from_server(DialogId dialog_id, MessageId max_message_id,
                                                        bool remove_from_dialog_list, bool allow_error,
                                                        uint64 logevent_id, Promise<Unit> &&promise) {
  LOG(INFO) << "Delete history in " << dialog_id << " up to " << max_message_id << " from server";

  if (logevent_id == 0 && G()->parameters().use_message_db) {
    DeleteDialogHistoryFromServerLogEvent logevent;
    logevent.dialog_id_ = dialog_id;
    logevent.max_message_id_ = max_message_id;
    logevent.remove_from_dialog_list_ = remove_from_dialog_list;

    auto storer = LogEventStorerImpl<DeleteDialogHistoryFromServerLogEvent>(logevent);
    logevent_id =
        BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::DeleteDialogHistoryFromServer, storer);
  }

  if (logevent_id != 0) {
    auto new_promise = PromiseCreator::lambda([logevent_id, promise = std::move(promise)](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }

      promise.set_result(std::move(result));
    });
    promise = std::move(new_promise);
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      td_->create_handler<DeleteHistoryQuery>(std::move(promise))
          ->send(dialog_id, max_message_id, remove_from_dialog_list);
      break;
    case DialogType::Channel:
      td_->create_handler<DeleteChannelHistoryQuery>(std::move(promise))
          ->send(dialog_id.get_channel_id(), max_message_id, allow_error);
      break;
    case DialogType::SecretChat:
      // TODO: use promise
      send_closure(G()->secret_chats_manager(), &SecretChatsManager::delete_all_messages,
                   dialog_id.get_secret_chat_id(), Promise<>());
      promise.set_value(Unit());
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
      break;
  }
}

void MessagesManager::find_messages_from_user(const unique_ptr<Message> &m, UserId user_id,
                                              vector<MessageId> &message_ids) {
  if (m == nullptr) {
    return;
  }

  find_messages_from_user(m->left, user_id, message_ids);

  if (m->sender_user_id == user_id) {
    message_ids.push_back(m->message_id);
  }

  find_messages_from_user(m->right, user_id, message_ids);
}

void MessagesManager::find_unread_mentions(const unique_ptr<Message> &m, vector<MessageId> &message_ids) {
  if (m == nullptr) {
    return;
  }

  find_unread_mentions(m->left, message_ids);

  if (m->contains_unread_mention) {
    message_ids.push_back(m->message_id);
  }

  find_unread_mentions(m->right, message_ids);
}

void MessagesManager::find_old_messages(const unique_ptr<Message> &m, MessageId max_message_id,
                                        vector<MessageId> &message_ids) {
  if (m == nullptr) {
    return;
  }

  find_old_messages(m->left, max_message_id, message_ids);

  if (m->message_id.get() <= max_message_id.get()) {
    message_ids.push_back(m->message_id);

    find_old_messages(m->right, max_message_id, message_ids);
  }
}

void MessagesManager::find_unloadable_messages(const Dialog *d, int32 unload_before_date, const unique_ptr<Message> &m,
                                               vector<MessageId> &message_ids, int32 &left_to_unload) const {
  if (m == nullptr) {
    return;
  }

  find_unloadable_messages(d, unload_before_date, m->left, message_ids, left_to_unload);

  if (can_unload_message(d, m.get())) {
    if (m->last_access_date <= unload_before_date) {
      message_ids.push_back(m->message_id);
    } else {
      left_to_unload++;
    }
  }

  find_unloadable_messages(d, unload_before_date, m->right, message_ids, left_to_unload);
}

void MessagesManager::delete_dialog_messages_from_user(DialogId dialog_id, UserId user_id, Promise<Unit> &&promise) {
  bool is_bot = td_->auth_manager_->is_bot();
  if (is_bot) {
    return promise.set_error(Status::Error(3, "Method is not available for bots"));
  }

  LOG(INFO) << "Receive deleteChatMessagesFromUser request to delete all messages in " << dialog_id << " from the user "
            << user_id;
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Write)) {
    return promise.set_error(Status::Error(3, "Not enough rights"));
  }

  if (!td_->contacts_manager_->have_input_user(user_id)) {
    return promise.set_error(Status::Error(3, "User not found"));
  }

  ChannelId channel_id;
  DialogParticipantStatus channel_status = DialogParticipantStatus::Left();
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(3, "All messages from a user can be deleted only in supergroup chats"));
    case DialogType::Channel: {
      channel_id = dialog_id.get_channel_id();
      auto channel_type = td_->contacts_manager_->get_channel_type(channel_id);
      if (channel_type != ChannelType::Megagroup) {
        return promise.set_error(Status::Error(3, "The method is available only for supergroup chats"));
      }
      channel_status = td_->contacts_manager_->get_channel_status(channel_id);
      if (!channel_status.can_delete_messages()) {
        return promise.set_error(Status::Error(5, "Need delete messages administator right in the supergroup chat"));
      }
      channel_id = dialog_id.get_channel_id();
      break;
    }
    case DialogType::None:
    default:
      UNREACHABLE();
      break;
  }
  CHECK(channel_id.is_valid());

  if (G()->parameters().use_message_db) {
    LOG(INFO) << "Delete all messages from " << user_id << " in " << dialog_id << " from database";
    G()->td_db()->get_messages_db_async()->delete_dialog_messages_from_user(dialog_id, user_id,
                                                                            Auto());  // TODO Promise
  }

  vector<MessageId> message_ids;
  find_messages_from_user(d->messages, user_id, message_ids);

  vector<int64> deleted_message_ids;
  bool need_update_dialog_pos = false;
  for (auto message_id : message_ids) {
    auto m = get_message(d, message_id);
    CHECK(m != nullptr);
    CHECK(m->sender_user_id == user_id);
    CHECK(m->message_id == message_id);
    if (can_delete_channel_message(channel_status, m, is_bot)) {
      deleted_message_ids.push_back(message_id.get());
      auto p = delete_message(d, message_id, true, &need_update_dialog_pos, "delete messages from user");
      CHECK(p.get() == m);
    }
  }

  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "delete_messages_from_user");
  }

  send_update_delete_messages(dialog_id, std::move(deleted_message_ids), true, false);

  delete_all_channel_messages_from_user_on_server(channel_id, user_id, 0, std::move(promise));
}

class MessagesManager::DeleteAllChannelMessagesFromUserOnServerLogEvent {
 public:
  ChannelId channel_id_;
  UserId user_id_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(channel_id_, storer);
    td::store(user_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(channel_id_, parser);
    td::parse(user_id_, parser);
  }
};

void MessagesManager::delete_all_channel_messages_from_user_on_server(ChannelId channel_id, UserId user_id,
                                                                      uint64 logevent_id, Promise<Unit> &&promise) {
  if (logevent_id == 0 && G()->parameters().use_chat_info_db) {
    DeleteAllChannelMessagesFromUserOnServerLogEvent logevent;
    logevent.channel_id_ = channel_id;
    logevent.user_id_ = user_id;

    auto storer = LogEventStorerImpl<DeleteAllChannelMessagesFromUserOnServerLogEvent>(logevent);
    logevent_id = BinlogHelper::add(G()->td_db()->get_binlog(),
                                    LogEvent::HandlerType::DeleteAllChannelMessagesFromUserOnServer, storer);
  }

  if (logevent_id != 0) {
    auto new_promise = PromiseCreator::lambda([logevent_id, promise = std::move(promise)](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }

      promise.set_result(std::move(result));
    });
    promise = std::move(new_promise);
  }

  td_->create_handler<DeleteUserHistoryQuery>(std::move(promise))->send(channel_id, user_id);
}

void MessagesManager::unload_dialog(DialogId dialog_id) {
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  vector<MessageId> to_unload_message_ids;
  int32 left_to_unload = 0;
  find_unloadable_messages(d, G()->unix_time_cached() - DIALOG_UNLOAD_DELAY + 2, d->messages, to_unload_message_ids,
                           left_to_unload);

  vector<int64> unloaded_message_ids;
  for (auto message_id : to_unload_message_ids) {
    unload_message(d, message_id);
    unloaded_message_ids.push_back(message_id.get());
  }

  if (!unloaded_message_ids.empty()) {
    if (!G()->parameters().use_message_db) {
      d->have_full_history = false;
    }

    send_closure_later(
        G()->td(), &Td::send_update,
        make_tl_object<td_api::updateDeleteMessages>(dialog_id.get(), std::move(unloaded_message_ids), false, true));
  }

  if (left_to_unload > 0) {
    LOG(INFO) << "Need to unload " << left_to_unload << " messages more in " << dialog_id;
    pending_unload_dialog_timeout_.add_timeout_in(d->dialog_id.get(), DIALOG_UNLOAD_DELAY);
  }
}

void MessagesManager::delete_all_dialog_messages(Dialog *d, bool remove_from_dialog_list, bool is_permanent) {
  CHECK(d != nullptr);
  if (is_debug_message_op_enabled()) {
    d->debug_message_op.emplace_back(Dialog::MessageOp::DeleteAll, MessageId(), -1, remove_from_dialog_list, false,
                                     false, "");
  }

  if (d->server_unread_count + d->local_unread_count > 0) {
    MessageId max_message_id =
        d->last_database_message_id.is_valid() ? d->last_database_message_id : d->last_new_message_id;
    if (max_message_id.is_valid()) {
      read_history_inbox(d->dialog_id, max_message_id, -1, "delete_all_dialog_messages");
    }
    if (d->server_unread_count != 0 || d->local_unread_count != 0) {
      set_dialog_last_read_inbox_message_id(d, MessageId::min(), 0, 0, true, "delete_all_dialog_messages");
    }
  }

  if (d->unread_mention_count > 0) {
    d->unread_mention_count = 0;
    d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] = 0;
    send_update_chat_unread_mention_count(d);
  }

  bool has_last_message_id = d->last_message_id != MessageId();
  int32 last_message_date = 0;
  MessageId last_clear_history_message_id;
  if (!remove_from_dialog_list) {
    if (has_last_message_id) {
      auto m = get_message(d, d->last_message_id);
      CHECK(m != nullptr);
      last_message_date = m->date;
      last_clear_history_message_id = d->last_message_id;
    } else {
      last_message_date = d->last_clear_history_date;
      last_clear_history_message_id = d->last_clear_history_message_id;
    }
  }

  vector<int64> deleted_message_ids;
  do_delete_all_dialog_messages(d, d->messages, deleted_message_ids);
  delete_all_dialog_messages_from_database(d->dialog_id, MessageId::max(), "delete_all_dialog_messages");
  if (is_permanent) {
    for (auto id : deleted_message_ids) {
      d->deleted_message_ids.insert(MessageId{id});
    }
  }

  if (d->reply_markup_message_id != MessageId()) {
    set_dialog_reply_markup(d, MessageId());
  }

  set_dialog_first_database_message_id(d, MessageId(), "delete_all_dialog_messages");
  set_dialog_last_database_message_id(d, MessageId(), "delete_all_dialog_messages");
  set_dialog_last_clear_history_date(d, last_message_date, last_clear_history_message_id, "delete_all_dialog_messages");
  d->last_read_all_mentions_message_id = MessageId();  // it is not needed anymore
  std::fill(d->message_count_by_index.begin(), d->message_count_by_index.end(), 0);

  if (has_last_message_id) {
    set_dialog_last_message_id(d, MessageId(), "delete_all_dialog_messages");
    send_update_chat_last_message(d, "delete_all_dialog_messages");
  }
  if (remove_from_dialog_list && d->pinned_order != DEFAULT_ORDER) {
    set_dialog_is_pinned(d, false);
  }
  update_dialog_pos(d, remove_from_dialog_list, "delete_all_dialog_messages");

  on_dialog_updated(d->dialog_id, "delete_all_dialog_messages");

  send_update_delete_messages(d->dialog_id, std::move(deleted_message_ids), is_permanent, false);
}

void MessagesManager::delete_dialog(DialogId dialog_id) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return;
  }

  delete_all_dialog_messages(d, true, false);
  if (dialog_id.get_type() != DialogType::SecretChat) {
    d->have_full_history = false;
    d->need_restore_reply_markup = true;
  }

  close_dialog(d);
}

void MessagesManager::read_all_dialog_mentions(DialogId dialog_id, Promise<Unit> &&promise) {
  bool is_bot = td_->auth_manager_->is_bot();
  if (is_bot) {
    return promise.set_error(Status::Error(3, "Method is not available for bots"));
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  LOG(INFO) << "Receive readAllChatMentions request in " << dialog_id << " with " << d->unread_mention_count
            << " unread mentions";
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(3, "Chat is not accessible"));
  }

  if (d->last_new_message_id.get() > d->last_read_all_mentions_message_id.get()) {
    d->last_read_all_mentions_message_id = d->last_new_message_id;
    on_dialog_updated(dialog_id, "read_all_mentions");
  }

  vector<MessageId> message_ids;
  find_unread_mentions(d->messages, message_ids);

  LOG(INFO) << "Found " << message_ids.size() << " messages with unread mentions in memory";
  bool is_update_sent = false;
  for (auto message_id : message_ids) {
    auto m = get_message(d, message_id);
    CHECK(m != nullptr);
    CHECK(m->contains_unread_mention);
    CHECK(m->message_id == message_id);
    m->contains_unread_mention = false;

    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateMessageMentionRead>(dialog_id.get(), m->message_id.get(), 0));
    is_update_sent = true;
    on_message_changed(d, m, "read_all_mentions");
  }

  if (d->unread_mention_count != 0) {
    d->unread_mention_count = 0;
    d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] = 0;
    if (!is_update_sent) {
      send_update_chat_unread_mention_count(d);
    } else {
      LOG(INFO) << "Update unread mention message count in " << dialog_id << " to " << d->unread_mention_count;
      on_dialog_updated(dialog_id, "read_all_mentions");
    }
  }

  read_all_dialog_mentions_on_server(dialog_id, 0, std::move(promise));
}

class MessagesManager::ReadAllDialogMentionsOnServerLogEvent {
 public:
  DialogId dialog_id_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id_, parser);
  }
};

void MessagesManager::read_all_dialog_mentions_on_server(DialogId dialog_id, uint64 logevent_id,
                                                         Promise<Unit> &&promise) {
  if (logevent_id == 0 && G()->parameters().use_message_db) {
    ReadAllDialogMentionsOnServerLogEvent logevent;
    logevent.dialog_id_ = dialog_id;

    auto storer = LogEventStorerImpl<ReadAllDialogMentionsOnServerLogEvent>(logevent);
    logevent_id =
        BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::ReadAllDialogMentionsOnServer, storer);
  }

  if (logevent_id != 0) {
    auto new_promise = PromiseCreator::lambda([logevent_id, promise = std::move(promise)](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }

      promise.set_result(std::move(result));
    });
    promise = std::move(new_promise);
  }

  td_->create_handler<ReadAllMentionsQuery>(std::move(promise))->send(dialog_id);
}

void MessagesManager::read_message_content_from_updates(MessageId message_id) {
  if (!message_id.is_valid() || !message_id.is_server()) {
    LOG(ERROR) << "Incoming update tries to read content of " << message_id;
    return;
  }

  Dialog *d = get_dialog_by_message_id(message_id);
  if (d != nullptr) {
    Message *m = get_message(d, message_id);
    CHECK(m != nullptr);
    read_message_content(d, m, false, "read_message_content_from_updates");
  }
}

void MessagesManager::read_channel_message_content_from_updates(Dialog *d, MessageId message_id) {
  if (!message_id.is_valid() || !message_id.is_server()) {
    LOG(ERROR) << "Incoming update tries to read content of " << message_id << " in " << d->dialog_id;
    return;
  }

  Message *m = get_message_force(d, message_id);
  if (m != nullptr) {
    read_message_content(d, m, false, "read_channel_message_content_from_updates");
  }
}

bool MessagesManager::update_opened_message_content(Message *m) {
  switch (m->content->get_id()) {
    case MessageVideoNote::ID: {
      auto content = static_cast<MessageVideoNote *>(m->content.get());
      if (content->is_viewed) {
        return false;
      }
      content->is_viewed = true;
      return true;
    }
    case MessageVoiceNote::ID: {
      auto content = static_cast<MessageVoiceNote *>(m->content.get());
      if (content->is_listened) {
        return false;
      }
      content->is_listened = true;
      return true;
    }
    default:
      return false;
  }
}

bool MessagesManager::read_message_content(Dialog *d, Message *m, bool is_local_read, const char *source) {
  CHECK(m != nullptr) << source;
  bool is_mention_read = update_message_contains_unread_mention(d, m, false, "read_message_content");
  bool is_content_read = update_opened_message_content(m) | ttl_on_open(d, m, Time::now(), is_local_read);

  if (is_mention_read || is_content_read) {
    on_message_changed(d, m, "read_message_content");
    if (is_content_read) {
      send_closure(G()->td(), &Td::send_update,
                   make_tl_object<td_api::updateMessageContentOpened>(d->dialog_id.get(), m->message_id.get()));
    }
    return true;
  }
  return false;
}

void MessagesManager::read_history_inbox(DialogId dialog_id, MessageId max_message_id, int32 unread_count,
                                         const char *source) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d != nullptr) {
    if (unread_count < 0) {
      if (!max_message_id.is_valid()) {
        LOG(ERROR) << "Receive read inbox update in " << dialog_id << " up to " << max_message_id << " from " << source;
        return;
      }
    } else {
      if (!max_message_id.is_valid() && max_message_id != MessageId()) {
        LOG(ERROR) << "Receive read inbox history in " << dialog_id << " up to " << max_message_id << " from "
                   << source;
        return;
      }
    }
    if (d->is_last_read_inbox_message_id_inited && max_message_id.get() <= d->last_read_inbox_message_id.get()) {
      LOG(INFO) << "Receive read inbox update in " << dialog_id << " up to " << max_message_id << " from " << source
                << ", but all messages have already been read up to " << d->last_read_inbox_message_id;
      return;
    }

    if (max_message_id != MessageId() && max_message_id.is_yet_unsent()) {
      LOG(ERROR) << "Try to update last read inbox message in " << dialog_id << " with " << max_message_id << " from "
                 << source;
      return;
    }

    if (unread_count > 0 && max_message_id.get() >= d->last_new_message_id.get() &&
        max_message_id.get() >= d->last_message_id.get() && max_message_id.get() >= d->last_database_message_id.get()) {
      LOG(INFO) << "Have unknown " << unread_count << " unread messages in " << dialog_id;
      unread_count = 0;
    }

    LOG_IF(INFO, d->last_new_message_id.is_valid() && max_message_id.get() > d->last_new_message_id.get() &&
                     max_message_id.is_server() && dialog_id.get_type() != DialogType::Channel &&
                     !running_get_difference_)
        << "Receive read inbox update up to unknown " << max_message_id << " in " << dialog_id << " from " << source
        << ". Last new is " << d->last_new_message_id << ", unread_count = " << unread_count
        << ". Possible only for deleted incoming message. " << td_->updates_manager_->get_state();

    if (dialog_id.get_type() == DialogType::SecretChat) {
      // TODO: protect with logevent
      suffix_load_till_message_id(
          d, d->last_read_inbox_message_id,
          PromiseCreator::lambda([actor_id = actor_id(this), dialog_id, from_message_id = max_message_id,
                                  till_message_id = d->last_read_inbox_message_id,
                                  timestamp = Time::now()](Result<Unit>) {
            send_closure(actor_id, &MessagesManager::ttl_read_history_inbox, dialog_id, from_message_id,
                         till_message_id, timestamp);
          }));
    }

    int32 local_unread_count = 0;
    int32 server_unread_count = 0;
    if (dialog_id != DialogId(td_->contacts_manager_->get_my_id("read_history_inbox"))) {
      MessagesConstIterator it(d, MessageId::max());
      while (*it != nullptr && (*it)->message_id.get() > max_message_id.get()) {
        if (!(*it)->is_outgoing) {
          if ((*it)->message_id.is_server()) {
            server_unread_count++;
          } else {
            CHECK((*it)->message_id.is_local());
            local_unread_count++;
          }
        }
        --it;
      }
    }
    if (unread_count >= 0) {
      if (unread_count < server_unread_count) {
        LOG(ERROR) << "Receive unread_count = " << unread_count << ", but have at least " << server_unread_count
                   << " unread messages in " << dialog_id;
      } else {
        server_unread_count = unread_count;
      }
    }

    set_dialog_last_read_inbox_message_id(d, max_message_id, server_unread_count, local_unread_count, true, source);
  } else {
    LOG(INFO) << "Receive read inbox about unknown " << dialog_id << " from " << source;
  }
}

void MessagesManager::read_history_outbox(DialogId dialog_id, MessageId max_message_id, int32 read_date) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d != nullptr) {
    if (!max_message_id.is_valid()) {
      LOG(ERROR) << "Receive read outbox update in " << dialog_id << " with " << max_message_id;
      return;
    }
    if (max_message_id.get() <= d->last_read_outbox_message_id.get()) {
      LOG(INFO) << "Receive read outbox update up to " << max_message_id
                << ", but all messages have already been read up to " << d->last_read_outbox_message_id;
      return;
    }

    if (max_message_id.is_yet_unsent()) {
      LOG(ERROR) << "Try to update last read outbox message with " << max_message_id;
      return;
    }

    // it is impossible for just sent outgoing messages because updates are ordered by pts
    LOG_IF(INFO, d->last_new_message_id.is_valid() && max_message_id.get() > d->last_new_message_id.get() &&
                     dialog_id.get_type() != DialogType::Channel)
        << "Receive read outbox update about unknown " << max_message_id << " in " << dialog_id << " with last new "
        << d->last_new_message_id << ". Possible only for deleted outgoing message. "
        << td_->updates_manager_->get_state();

    if (dialog_id.get_type() == DialogType::SecretChat) {
      double server_time = Time::now();
      double read_time = server_time;
      if (read_date <= 0) {
        LOG(ERROR) << "Receive wrong read date " << read_date << " in " << dialog_id;
      } else if (read_date < server_time) {
        read_time = read_date;
      }
      // TODO: protect with logevent
      suffix_load_till_message_id(
          d, d->last_read_outbox_message_id,
          PromiseCreator::lambda([actor_id = actor_id(this), dialog_id, from_message_id = max_message_id,
                                  till_message_id = d->last_read_outbox_message_id, read_time](Result<Unit>) {
            send_closure(actor_id, &MessagesManager::ttl_read_history_outbox, dialog_id, from_message_id,
                         till_message_id, read_time);
          }));
    }

    set_dialog_last_read_outbox_message_id(d, max_message_id);
  } else {
    LOG(INFO) << "Receive read outbox update about unknown " << dialog_id;
  }
}

void MessagesManager::recalc_unread_message_count() {
  if (td_->auth_manager_->is_bot() || !need_unread_count_recalc_) {
    return;
  }
  need_unread_count_recalc_ = false;
  is_unread_count_inited_ = true;

  int32 total_count = 0;
  int32 muted_count = 0;
  for (auto &dialog_date : ordered_server_dialogs_) {
    auto dialog_id = dialog_date.get_dialog_id();
    Dialog *d = get_dialog(dialog_id);
    CHECK(d != nullptr);
    int unread_count = d->server_unread_count + d->local_unread_count;
    if (d->order != DEFAULT_ORDER && unread_count > 0) {
      total_count += unread_count;
      if (d->notification_settings.mute_until != 0) {
        muted_count += unread_count;
      } else {
        LOG(DEBUG) << "Have " << unread_count << " messages in unmuted " << dialog_id;
      }
    }
  }

  if (unread_message_total_count_ != total_count || unread_message_muted_count_ != muted_count) {
    unread_message_total_count_ = total_count;
    unread_message_muted_count_ = muted_count;
    send_update_unread_message_count(DialogId(), true, "recalc_unread_message_count");
  }
}

void MessagesManager::set_dialog_last_read_inbox_message_id(Dialog *d, MessageId message_id, int32 server_unread_count,
                                                            int32 local_unread_count, bool force_update,
                                                            const char *source) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  CHECK(d != nullptr);
  LOG(INFO) << "Update last read inbox message in " << d->dialog_id << " from " << d->last_read_inbox_message_id
            << " to " << message_id << " and update unread message count from " << d->server_unread_count << " + "
            << d->local_unread_count << " to " << server_unread_count << " + " << local_unread_count << " from "
            << source;
  if (message_id != MessageId::min()) {
    d->last_read_inbox_message_id = message_id;
    d->is_last_read_inbox_message_id_inited = true;
  }
  int32 old_unread_count = d->server_unread_count + d->local_unread_count;
  d->server_unread_count = server_unread_count;
  d->local_unread_count = local_unread_count;
  int32 new_unread_count = d->server_unread_count + d->local_unread_count;
  int32 delta = new_unread_count - old_unread_count;
  if (delta != 0 && d->order != DEFAULT_ORDER && is_unread_count_inited_) {
    unread_message_total_count_ += delta;
    if (d->notification_settings.mute_until != 0) {
      unread_message_muted_count_ += delta;
    }
    send_update_unread_message_count(d->dialog_id, force_update, source);
  }

  send_update_chat_read_inbox(d, force_update, source);
}

void MessagesManager::set_dialog_last_read_outbox_message_id(Dialog *d, MessageId message_id) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  CHECK(d != nullptr);
  LOG(INFO) << "Update last read outbox message in " << d->dialog_id << " from " << d->last_read_outbox_message_id
            << " to " << message_id;
  d->last_read_outbox_message_id = message_id;
  d->is_last_read_outbox_message_id_inited = true;
  send_update_chat_read_outbox(d);
}

void MessagesManager::set_dialog_max_unavailable_message_id(DialogId dialog_id, MessageId max_unavailable_message_id,
                                                            bool from_update, const char *source) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d != nullptr) {
    if (d->last_new_message_id.is_valid() && max_unavailable_message_id.get() > d->last_new_message_id.get()) {
      LOG(ERROR) << "Tried to set " << dialog_id << " max unavailable message id to " << max_unavailable_message_id
                 << " from " << source << ", but last new message id is " << d->last_new_message_id;
      max_unavailable_message_id = d->last_new_message_id;
    }

    if (d->max_unavailable_message_id == max_unavailable_message_id) {
      return;
    }

    if (max_unavailable_message_id.is_valid() && max_unavailable_message_id.is_yet_unsent()) {
      LOG(ERROR) << "Try to update " << dialog_id << " last read outbox message with " << max_unavailable_message_id
                 << " from " << source;
      return;
    }
    LOG(INFO) << "Set max unavailable message id to " << max_unavailable_message_id << " in " << dialog_id << " from "
              << source;

    on_dialog_updated(dialog_id, "set_dialog_max_unavailable_message_id");

    if (d->max_unavailable_message_id.get() > max_unavailable_message_id.get()) {
      d->max_unavailable_message_id = max_unavailable_message_id;
      return;
    }

    d->max_unavailable_message_id = max_unavailable_message_id;

    vector<MessageId> message_ids;
    find_old_messages(d->messages, max_unavailable_message_id, message_ids);

    vector<int64> deleted_message_ids;
    bool need_update_dialog_pos = false;
    for (auto message_id : message_ids) {
      if (message_id.is_yet_unsent()) {
        continue;
      }

      auto m = get_message(d, message_id);
      CHECK(m != nullptr);
      CHECK(m->message_id.get() <= max_unavailable_message_id.get());
      CHECK(m->message_id == message_id);
      deleted_message_ids.push_back(message_id.get());
      auto p =
          delete_message(d, message_id, !from_update, &need_update_dialog_pos, "set_dialog_max_unavailable_message_id");
      CHECK(p.get() == m);
    }

    if (need_update_dialog_pos) {
      send_update_chat_last_message(d, "set_dialog_max_unavailable_message_id");
    }

    send_update_delete_messages(dialog_id, std::move(deleted_message_ids), !from_update, false);

    if (d->server_unread_count + d->local_unread_count > 0) {
      read_history_inbox(dialog_id, max_unavailable_message_id, -1, "set_dialog_max_unavailable_message_id");
    }
  } else {
    LOG(INFO) << "Receive max unavailable message identifier in unknown " << dialog_id << " from " << source;
  }
}

tl_object_ptr<td_api::MessageContent> MessagesManager::get_message_content_object(const MessageContent *content,
                                                                                  int32 message_date,
                                                                                  bool is_content_secret) const {
  CHECK(content != nullptr);
  switch (content->get_id()) {
    case MessageAnimation::ID: {
      const MessageAnimation *m = static_cast<const MessageAnimation *>(content);
      return make_tl_object<td_api::messageAnimation>(
          td_->animations_manager_->get_animation_object(m->file_id, "get_message_content_object"),
          get_formatted_text_object(m->caption), is_content_secret);
    }
    case MessageAudio::ID: {
      const MessageAudio *m = static_cast<const MessageAudio *>(content);
      return make_tl_object<td_api::messageAudio>(td_->audios_manager_->get_audio_object(m->file_id),
                                                  get_formatted_text_object(m->caption));
    }
    case MessageContact::ID: {
      const MessageContact *m = static_cast<const MessageContact *>(content);
      return make_tl_object<td_api::messageContact>(m->contact.get_contact_object());
    }
    case MessageDocument::ID: {
      const MessageDocument *m = static_cast<const MessageDocument *>(content);
      return make_tl_object<td_api::messageDocument>(td_->documents_manager_->get_document_object(m->file_id),
                                                     get_formatted_text_object(m->caption));
    }
    case MessageGame::ID: {
      const MessageGame *m = static_cast<const MessageGame *>(content);
      return make_tl_object<td_api::messageGame>(m->game.get_game_object(td_));
    }
    case MessageInvoice::ID: {
      const MessageInvoice *m = static_cast<const MessageInvoice *>(content);
      return make_tl_object<td_api::messageInvoice>(
          m->title, m->description, get_photo_object(td_->file_manager_.get(), &m->photo), m->invoice.currency,
          m->total_amount, m->start_parameter, m->invoice.is_test, m->invoice.need_shipping_address,
          m->receipt_message_id.get());
    }
    case MessageLiveLocation::ID: {
      const MessageLiveLocation *m = static_cast<const MessageLiveLocation *>(content);
      auto passed = max(G()->unix_time_cached() - message_date, 0);
      return make_tl_object<td_api::messageLocation>(m->location.get_location_object(), m->period,
                                                     max(0, m->period - passed));
    }
    case MessageLocation::ID: {
      const MessageLocation *m = static_cast<const MessageLocation *>(content);
      return make_tl_object<td_api::messageLocation>(m->location.get_location_object(), 0, 0);
    }
    case MessagePhoto::ID: {
      const MessagePhoto *m = static_cast<const MessagePhoto *>(content);
      return make_tl_object<td_api::messagePhoto>(get_photo_object(td_->file_manager_.get(), &m->photo),
                                                  get_formatted_text_object(m->caption), is_content_secret);
    }
    case MessageSticker::ID: {
      const MessageSticker *m = static_cast<const MessageSticker *>(content);
      return make_tl_object<td_api::messageSticker>(td_->stickers_manager_->get_sticker_object(m->file_id));
    }
    case MessageText::ID: {
      const MessageText *m = static_cast<const MessageText *>(content);
      return make_tl_object<td_api::messageText>(get_formatted_text_object(m->text),
                                                 td_->web_pages_manager_->get_web_page_object(m->web_page_id));
    }
    case MessageUnsupported::ID: {
      return make_tl_object<td_api::messageUnsupported>();
    }
    case MessageVenue::ID: {
      const MessageVenue *m = static_cast<const MessageVenue *>(content);
      return make_tl_object<td_api::messageVenue>(m->venue.get_venue_object());
    }
    case MessageVideo::ID: {
      const MessageVideo *m = static_cast<const MessageVideo *>(content);
      return make_tl_object<td_api::messageVideo>(td_->videos_manager_->get_video_object(m->file_id),
                                                  get_formatted_text_object(m->caption), is_content_secret);
    }
    case MessageVideoNote::ID: {
      const MessageVideoNote *m = static_cast<const MessageVideoNote *>(content);
      return make_tl_object<td_api::messageVideoNote>(td_->video_notes_manager_->get_video_note_object(m->file_id),
                                                      m->is_viewed, is_content_secret);
    }
    case MessageVoiceNote::ID: {
      const MessageVoiceNote *m = static_cast<const MessageVoiceNote *>(content);
      return make_tl_object<td_api::messageVoiceNote>(td_->voice_notes_manager_->get_voice_note_object(m->file_id),
                                                      get_formatted_text_object(m->caption), m->is_listened);
    }
    case MessageChatCreate::ID: {
      const MessageChatCreate *m = static_cast<const MessageChatCreate *>(content);
      return make_tl_object<td_api::messageBasicGroupChatCreate>(
          m->title, td_->contacts_manager_->get_user_ids_object(m->participant_user_ids));
    }
    case MessageChatChangeTitle::ID: {
      const MessageChatChangeTitle *m = static_cast<const MessageChatChangeTitle *>(content);
      return make_tl_object<td_api::messageChatChangeTitle>(m->title);
    }
    case MessageChatChangePhoto::ID: {
      const MessageChatChangePhoto *m = static_cast<const MessageChatChangePhoto *>(content);
      return make_tl_object<td_api::messageChatChangePhoto>(get_photo_object(td_->file_manager_.get(), &m->photo));
    }
    case MessageChatDeletePhoto::ID:
      return make_tl_object<td_api::messageChatDeletePhoto>();
    case MessageChatDeleteHistory::ID:
      return make_tl_object<td_api::messageUnsupported>();
    case MessageChatAddUsers::ID: {
      const MessageChatAddUsers *m = static_cast<const MessageChatAddUsers *>(content);
      return make_tl_object<td_api::messageChatAddMembers>(td_->contacts_manager_->get_user_ids_object(m->user_ids));
    }
    case MessageChatJoinedByLink::ID:
      return make_tl_object<td_api::messageChatJoinByLink>();
    case MessageChatDeleteUser::ID: {
      const MessageChatDeleteUser *m = static_cast<const MessageChatDeleteUser *>(content);
      return make_tl_object<td_api::messageChatDeleteMember>(
          td_->contacts_manager_->get_user_id_object(m->user_id, "messageChatDeleteMember"));
    }
    case MessageChatMigrateTo::ID: {
      const MessageChatMigrateTo *m = static_cast<const MessageChatMigrateTo *>(content);
      return make_tl_object<td_api::messageChatUpgradeTo>(
          td_->contacts_manager_->get_supergroup_id_object(m->migrated_to_channel_id, "messageChatUpgradeTo"));
    }
    case MessageChannelCreate::ID: {
      const MessageChannelCreate *m = static_cast<const MessageChannelCreate *>(content);
      return make_tl_object<td_api::messageSupergroupChatCreate>(m->title);
    }
    case MessageChannelMigrateFrom::ID: {
      const MessageChannelMigrateFrom *m = static_cast<const MessageChannelMigrateFrom *>(content);
      return make_tl_object<td_api::messageChatUpgradeFrom>(
          m->title,
          td_->contacts_manager_->get_basic_group_id_object(m->migrated_from_chat_id, "messageChatUpgradeFrom"));
    }
    case MessagePinMessage::ID: {
      const MessagePinMessage *m = static_cast<const MessagePinMessage *>(content);
      return make_tl_object<td_api::messagePinMessage>(m->message_id.get());
    }
    case MessageGameScore::ID: {
      const MessageGameScore *m = static_cast<const MessageGameScore *>(content);
      return make_tl_object<td_api::messageGameScore>(m->game_message_id.get(), m->game_id, m->score);
    }
    case MessageScreenshotTaken::ID:
      return make_tl_object<td_api::messageScreenshotTaken>();
    case MessageChatSetTtl::ID: {
      const MessageChatSetTtl *m = static_cast<const MessageChatSetTtl *>(content);
      return make_tl_object<td_api::messageChatSetTtl>(m->ttl);
    }
    case MessageCall::ID: {
      const MessageCall *m = static_cast<const MessageCall *>(content);
      return make_tl_object<td_api::messageCall>(get_call_discard_reason_object(m->discard_reason), m->duration);
    }
    case MessagePaymentSuccessful::ID: {
      const MessagePaymentSuccessful *m = static_cast<const MessagePaymentSuccessful *>(content);
      if (td_->auth_manager_->is_bot()) {
        return make_tl_object<td_api::messagePaymentSuccessfulBot>(
            m->invoice_message_id.get(), m->currency, m->total_amount, m->invoice_payload, m->shipping_option_id,
            get_order_info_object(m->order_info), m->telegram_payment_charge_id, m->provider_payment_charge_id);
      } else {
        return make_tl_object<td_api::messagePaymentSuccessful>(m->invoice_message_id.get(), m->currency,
                                                                m->total_amount);
      }
    }
    case MessageContactRegistered::ID:
      return make_tl_object<td_api::messageContactRegistered>();
    case MessageExpiredPhoto::ID:
      return make_tl_object<td_api::messageExpiredPhoto>();
    case MessageExpiredVideo::ID:
      return make_tl_object<td_api::messageExpiredVideo>();
    case MessageCustomServiceAction::ID: {
      const MessageCustomServiceAction *m = static_cast<const MessageCustomServiceAction *>(content);
      return make_tl_object<td_api::messageCustomServiceAction>(m->message);
    }
    case MessageWebsiteConnected::ID: {
      const MessageWebsiteConnected *m = static_cast<const MessageWebsiteConnected *>(content);
      return make_tl_object<td_api::messageWebsiteConnected>(m->domain_name);
    }
    default:
      UNREACHABLE();
      return nullptr;
  }
  UNREACHABLE();
  return nullptr;
}

MessageId MessagesManager::get_message_id(const tl_object_ptr<telegram_api::Message> &message_ptr) {
  int32 constructor_id = message_ptr->get_id();
  switch (constructor_id) {
    case telegram_api::messageEmpty::ID: {
      auto message = static_cast<const telegram_api::messageEmpty *>(message_ptr.get());
      return MessageId(ServerMessageId(message->id_));
    }
    case telegram_api::message::ID: {
      auto message = static_cast<const telegram_api::message *>(message_ptr.get());
      return MessageId(ServerMessageId(message->id_));
    }
    case telegram_api::messageService::ID: {
      auto message = static_cast<const telegram_api::messageService *>(message_ptr.get());
      return MessageId(ServerMessageId(message->id_));
    }
    default:
      UNREACHABLE();
      return MessageId();
  }
}

DialogId MessagesManager::get_message_dialog_id(const tl_object_ptr<telegram_api::Message> &message_ptr) const {
  return get_full_message_id(message_ptr).get_dialog_id();
}

FullMessageId MessagesManager::get_full_message_id(const tl_object_ptr<telegram_api::Message> &message_ptr) const {
  int32 constructor_id = message_ptr->get_id();
  DialogId dialog_id;
  MessageId message_id;
  UserId sender_user_id;
  switch (constructor_id) {
    case telegram_api::messageEmpty::ID: {
      auto message = static_cast<const telegram_api::messageEmpty *>(message_ptr.get());
      LOG(INFO) << "Receive MessageEmpty";
      message_id = MessageId(ServerMessageId(message->id_));
      break;
    }
    case telegram_api::message::ID: {
      auto message = static_cast<const telegram_api::message *>(message_ptr.get());
      dialog_id = DialogId(message->to_id_);
      message_id = MessageId(ServerMessageId(message->id_));
      if (message->flags_ & MESSAGE_FLAG_HAS_FROM_ID) {
        sender_user_id = UserId(message->from_id_);
      }
      break;
    }
    case telegram_api::messageService::ID: {
      auto message = static_cast<const telegram_api::messageService *>(message_ptr.get());
      dialog_id = DialogId(message->to_id_);
      message_id = MessageId(ServerMessageId(message->id_));
      if (message->flags_ & MESSAGE_FLAG_HAS_FROM_ID) {
        sender_user_id = UserId(message->from_id_);
      }
      break;
    }
    default:
      UNREACHABLE();
      break;
  }

  UserId my_id = td_->contacts_manager_->get_my_id("get_full_message_id");
  DialogId my_dialog_id = DialogId(my_id);
  if (dialog_id == my_dialog_id) {
    LOG_IF(ERROR, !sender_user_id.is_valid()) << "Receive invalid " << sender_user_id;
    dialog_id = DialogId(sender_user_id);
  }
  return {dialog_id, message_id};
}

int32 MessagesManager::get_message_date(const tl_object_ptr<telegram_api::Message> &message_ptr) {
  int32 constructor_id = message_ptr->get_id();
  switch (constructor_id) {
    case telegram_api::messageEmpty::ID:
      return 0;
    case telegram_api::message::ID: {
      auto message = static_cast<const telegram_api::message *>(message_ptr.get());
      return message->date_;
    }
    case telegram_api::messageService::ID: {
      auto message = static_cast<const telegram_api::messageService *>(message_ptr.get());
      return message->date_;
    }
    default:
      UNREACHABLE();
      return 0;
  }
}

void MessagesManager::ttl_read_history_inbox(DialogId dialog_id, MessageId from_message_id, MessageId till_message_id,
                                             double timestamp) {
  auto *d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  auto now = Time::now();
  for (auto it = MessagesIterator(d, from_message_id); *it && (*it)->message_id.get() >= till_message_id.get(); --it) {
    auto *message = *it;
    if (!message->is_outgoing && !message->message_id.is_yet_unsent()) {
      ttl_on_view(d, message, timestamp, now);
    }
  }
}
void MessagesManager::ttl_read_history_outbox(DialogId dialog_id, MessageId from_message_id, MessageId till_message_id,
                                              double timestamp) {
  auto *d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  auto now = Time::now();
  for (auto it = MessagesIterator(d, from_message_id); *it && (*it)->message_id.get() >= till_message_id.get(); --it) {
    auto *message = *it;
    if (message->is_outgoing && !message->message_id.is_yet_unsent()) {
      ttl_on_view(d, message, timestamp, now);
    }
  }
}

void MessagesManager::ttl_on_view(const Dialog *d, Message *message, double view_date, double now) {
  if (message->ttl > 0 && message->ttl_expires_at == 0 && !message->is_content_secret) {
    message->ttl_expires_at = message->ttl + view_date;
    ttl_register_message(d->dialog_id, message, now);
    on_message_changed(d, message, "ttl_on_view");
  }
}

bool MessagesManager::ttl_on_open(Dialog *d, Message *message, double now, bool is_local_read) {
  if (message->ttl > 0 && message->ttl_expires_at == 0) {
    if (!is_local_read && d->dialog_id.get_type() != DialogType::SecretChat) {
      on_message_ttl_expired(d, message);
    } else {
      message->ttl_expires_at = message->ttl + now;
      ttl_register_message(d->dialog_id, message, now);
    }
    return true;
  }
  return false;
}

void MessagesManager::ttl_register_message(DialogId dialog_id, const Message *message, double now) {
  if (message->ttl_expires_at == 0) {
    return;
  }
  auto it_flag = ttl_nodes_.insert(TtlNode(dialog_id, message->message_id));
  CHECK(it_flag.second);
  auto it = it_flag.first;

  ttl_heap_.insert(message->ttl_expires_at, it->as_heap_node());
  ttl_update_timeout(now);
}

void MessagesManager::ttl_unregister_message(DialogId dialog_id, const Message *message, double now) {
  if (message->ttl_expires_at == 0) {
    return;
  }

  TtlNode ttl_node(dialog_id, message->message_id);
  auto it = ttl_nodes_.find(ttl_node);
  CHECK(it != ttl_nodes_.end());
  auto *heap_node = it->as_heap_node();
  if (heap_node->in_heap()) {
    ttl_heap_.erase(heap_node);
  }
  ttl_nodes_.erase(it);
  ttl_update_timeout(now);
}

void MessagesManager::ttl_loop(double now) {
  std::unordered_map<DialogId, std::vector<MessageId>, DialogIdHash> to_delete;
  while (!ttl_heap_.empty() && ttl_heap_.top_key() < now) {
    auto full_message_id = TtlNode::from_heap_node(ttl_heap_.pop())->full_message_id;
    auto dialog_id = full_message_id.get_dialog_id();
    if (dialog_id.get_type() == DialogType::SecretChat) {
      to_delete[dialog_id].push_back(full_message_id.get_message_id());
    } else {
      auto d = get_dialog(dialog_id);
      CHECK(d != nullptr);
      auto m = get_message(d, full_message_id.get_message_id());
      CHECK(m != nullptr);
      on_message_ttl_expired(d, m);
      on_message_changed(d, m, "ttl_loop");
    }
  }
  for (auto &it : to_delete) {
    delete_dialog_messages_from_updates(it.first, it.second);
  }
  ttl_update_timeout(now);
}

void MessagesManager::ttl_update_timeout(double now) {
  if (ttl_heap_.empty()) {
    if (!ttl_slot_.empty()) {
      ttl_slot_.cancel_timeout();
    }
    return;
  }
  ttl_slot_.set_event(EventCreator::yield(actor_id()));
  ttl_slot_.set_timeout_in(ttl_heap_.top_key() - now);
}

void MessagesManager::on_message_ttl_expired(Dialog *d, Message *message) {
  CHECK(d != nullptr);
  CHECK(message != nullptr);
  CHECK(message->ttl > 0);
  CHECK(d->dialog_id.get_type() != DialogType::SecretChat);
  ttl_unregister_message(d->dialog_id, message, Time::now());
  on_message_ttl_expired_impl(d, message);
  send_update_message_content(d->dialog_id, message->message_id, message->content.get(), message->date,
                              message->is_content_secret, "on_message_ttl_expired");
}

void MessagesManager::on_message_ttl_expired_impl(Dialog *d, Message *message) {
  CHECK(d != nullptr);
  CHECK(message != nullptr);
  CHECK(message->ttl > 0);
  CHECK(d->dialog_id.get_type() != DialogType::SecretChat);
  delete_message_files(message);
  switch (message->content->get_id()) {
    case MessagePhoto::ID:
      message->content = make_unique<MessageExpiredPhoto>();
      break;
    case MessageVideo::ID:
      message->content = make_unique<MessageExpiredVideo>();
      break;
    default:
      UNREACHABLE();
  }
  message->ttl = 0;
  message->ttl_expires_at = 0;
  if (message->reply_markup != nullptr) {
    if (message->reply_markup->type != ReplyMarkup::Type::InlineKeyboard) {
      if (!td_->auth_manager_->is_bot()) {
        if (d->reply_markup_message_id == message->message_id) {
          set_dialog_reply_markup(d, MessageId());
        }
      }
      message->had_reply_markup = true;
    }
    message->reply_markup = nullptr;
  }
  update_message_contains_unread_mention(d, message, false, "on_message_ttl_expired_impl");
  message->contains_mention = false;
  message->reply_to_message_id = MessageId();
  message->is_content_secret = false;
}

void MessagesManager::loop() {
  auto token = get_link_token();
  if (token == YieldType::TtlDb) {
    ttl_db_loop(G()->server_time());
  } else {
    ttl_loop(Time::now());
  }
}

void MessagesManager::tear_down() {
  parent_.reset();
}

void MessagesManager::start_up() {
  always_wait_for_mailbox();

  if (G()->parameters().use_message_db) {
    ttl_db_loop_start(G()->server_time());
  }
  load_calls_db_state();

  vector<NotificationSettingsScope> scopes{NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS,
                                           NOTIFICATION_SETTINGS_FOR_GROUP_CHATS, NOTIFICATION_SETTINGS_FOR_ALL_CHATS};
  for (auto scope : scopes) {
    auto notification_settings_string =
        G()->td_db()->get_binlog_pmc()->get(get_notification_settings_scope_database_key(scope));
    if (!notification_settings_string.empty()) {
      NotificationSettings notification_settings;
      log_event_parse(notification_settings, notification_settings_string).ensure();
      if (!notification_settings.is_synchronized) {
        continue;
      }

      NotificationSettings *current_settings = get_notification_settings(scope, true);
      CHECK(current_settings != nullptr);
      update_notification_settings(scope, current_settings, notification_settings);
    }
  }

  /*
  FI LE *f = std::f open("error.txt", "r");
  if (f != nullptr) {
    DialogId dialog_id(ChannelId(123456));
    force_create_dialog(dialog_id, "test");
    Dialog *d = get_dialog(dialog_id);
    CHECK(d != nullptr);

    delete_all_dialog_messages(d, true, false);

    d->last_new_message_id = MessageId();
    d->last_read_inbox_message_id = MessageId();
    d->last_read_outbox_message_id = MessageId();
    d->is_last_read_inbox_message_id_inited = false;
    d->is_last_read_outbox_message_id_inited = false;

    struct MessageBasicInfo {
      MessageId message_id;
      bool have_previous;
      bool have_next;
    };
    vector<MessageBasicInfo> messages_info;
    std::function<void(Message *m)> get_messages_info = [&](Message *m) {
      if (m == nullptr) {
        return;
      }
      get_messages_info(m->left.get());
      messages_info.push_back(MessageBasicInfo{m->message_id, m->have_previous, m->have_next});
      get_messages_info(m->right.get());
    };

    char buf[1280];
    while (std::f gets(buf, sizeof(buf), f) != nullptr) {
      Slice log_string(buf, std::strlen(buf));
      Slice op = log_string.substr(0, log_string.find(' '));
      if (op != "MessageOpAdd" && op != "MessageOpDelete") {
        LOG(ERROR) << "Unsupported op " << op;
        continue;
      }
      log_string.remove_prefix(log_string.find(' ') + 1);

      if (!begins_with(log_string, "at ")) {
        LOG(ERROR) << "Date expected, found " << log_string;
        continue;
      }
      log_string.remove_prefix(3);
      auto date_slice = log_string.substr(0, log_string.find(' '));
      log_string.remove_prefix(date_slice.size());

      bool is_server = false;
      if (begins_with(log_string, " server message ")) {
        log_string.remove_prefix(16);
        is_server = true;
      } else if (begins_with(log_string, " yet unsent message ")) {
        log_string.remove_prefix(20);
      } else if (begins_with(log_string, " local message ")) {
        log_string.remove_prefix(15);
      } else {
        LOG(ERROR) << "Message id expected, found " << log_string;
        continue;
      }

      auto server_message_id = to_integer<int32>(log_string);
      auto add = 0;
      if (!is_server) {
        log_string.remove_prefix(log_string.find('.') + 1);
        add = to_integer<int32>(log_string);
      }
      log_string.remove_prefix(log_string.find(' ') + 1);

      auto message_id = MessageId((static_cast<int64>(server_message_id) << MessageId::SERVER_ID_SHIFT) + add);

      int32 content_type = to_integer<int32>(log_string);
      log_string.remove_prefix(log_string.find(' ') + 1);

      auto read_bool = [](Slice &str) {
        if (begins_with(str, "true ")) {
          str.remove_prefix(5);
          return true;
        }
        if (begins_with(str, "false ")) {
          str.remove_prefix(6);
          return false;
        }
        LOG(ERROR) << "Bool expected, found " << str;
        return false;
      };

      bool from_update = read_bool(log_string);
      bool have_previous = read_bool(log_string);
      bool have_next = read_bool(log_string);

      CHECK(content_type != MessageChatDeleteHistory::ID);  // not supported
      if (op == "MessageOpAdd") {
        auto m = make_unique<Message>();
        m->random_y = get_random_y(message_id);
        m->message_id = message_id;
        m->date = G()->unix_time();
        m->content = make_unique<MessageText>(FormattedText{"text", vector<MessageEntity>()}, WebPageId());

        m->have_previous = have_previous;
        m->have_next = have_next;

        bool need_update = false;
        bool need_update_dialog_pos = false;
        if (add_message_to_dialog(dialog_id, std::move(m), from_update, &need_update, &need_update_dialog_pos,
                                  "Unknown source") == nullptr) {
          LOG(ERROR) << "Can't add message " << message_id;
        }
      } else {
        bool need_update_dialog_pos = false;
        auto m = delete_message(d, message_id, true, &need_update_dialog_pos, "Unknown source");
        CHECK(m != nullptr);
      }

      messages_info.clear();
      get_messages_info(d->messages.get());

      for (size_t i = 0; i + 1 < messages_info.size(); i++) {
        if (messages_info[i].have_next != messages_info[i + 1].have_previous) {
          LOG(ERROR) << messages_info[i].message_id << " has have_next = " << messages_info[i].have_next << ", but "
                     << messages_info[i + 1].message_id
                     << " has have_previous = " << messages_info[i + 1].have_previous;
        }
      }
      if (!messages_info.empty()) {
        if (messages_info.back().have_next != false) {
          LOG(ERROR) << messages_info.back().message_id << " has have_next = true, but there is no next message";
        }
        if (messages_info[0].have_previous != false) {
          LOG(ERROR) << messages_info[0].message_id << " has have_previous = true, but there is no previous message";
        }
      }
    }

    messages_info.clear();
    get_messages_info(d->messages.get());
    for (auto &info : messages_info) {
      bool need_update_dialog_pos = false;
      auto m = delete_message(d, info.message_id, true, &need_update_dialog_pos, "Unknown source");
      CHECK(m != nullptr);
    }

    std::f close(f);
  }
  */
}

void MessagesManager::ttl_db_loop_start(double server_now) {
  ttl_db_expire_from_ = 0;
  ttl_db_expire_till_ = static_cast<int32>(server_now) + 15 /* 15 seconds */;
  ttl_db_has_query_ = false;

  ttl_db_loop(server_now);
}

void MessagesManager::ttl_db_loop(double server_now) {
  LOG(INFO) << "ttl_db: loop " << tag("expire_from", ttl_db_expire_from_) << tag("expire_till", ttl_db_expire_till_)
            << tag("has_query", ttl_db_has_query_);
  if (ttl_db_has_query_) {
    return;
  }

  auto now = static_cast<int32>(server_now);

  if (ttl_db_expire_till_ < 0) {
    LOG(INFO) << "ttl_db: finished";
    return;
  }

  if (now < ttl_db_expire_from_) {
    ttl_db_slot_.set_event(EventCreator::yield(actor_shared(this, YieldType::TtlDb)));
    auto wakeup_in = ttl_db_expire_from_ - server_now;
    ttl_db_slot_.set_timeout_in(wakeup_in);
    LOG(INFO) << "ttl_db: " << tag("wakeup in", wakeup_in);
    return;
  }

  ttl_db_has_query_ = true;
  int32 limit = 50;
  LOG(INFO) << "ttl_db: send query " << tag("expire_from", ttl_db_expire_from_)
            << tag("expire_till", ttl_db_expire_till_) << tag("limit", limit);
  G()->td_db()->get_messages_db_async()->get_expiring_messages(
      ttl_db_expire_from_, ttl_db_expire_till_, limit,
      PromiseCreator::lambda(
          [actor_id = actor_id(this)](Result<std::pair<std::vector<std::pair<DialogId, BufferSlice>>, int32>> result) {
            send_closure(actor_id, &MessagesManager::ttl_db_on_result, std::move(result), false);
          }));
}

void MessagesManager::ttl_db_on_result(Result<std::pair<std::vector<std::pair<DialogId, BufferSlice>>, int32>> r_result,
                                       bool dummy) {
  auto result = r_result.move_as_ok();
  ttl_db_has_query_ = false;
  ttl_db_expire_from_ = ttl_db_expire_till_;
  ttl_db_expire_till_ = result.second;

  LOG(INFO) << "ttl_db: query result " << tag("new expire_till", ttl_db_expire_till_)
            << tag("got messages", result.first.size());
  for (auto &dialog_message : result.first) {
    on_get_message_from_database(dialog_message.first, get_dialog_force(dialog_message.first), dialog_message.second);
  }
  ttl_db_loop(G()->server_time());
}

void MessagesManager::on_send_secret_message_error(int64 random_id, Status error, Promise<> promise) {
  promise.set_value(Unit());  // TODO: set after error is saved
  LOG(INFO) << "Receive error for SecretChatsManager::send_message: " << error;

  auto it = being_sent_messages_.find(random_id);
  if (it != being_sent_messages_.end()) {
    auto full_message_id = it->second;
    auto *message = get_message(full_message_id);
    if (message != nullptr) {
      auto file_id = get_message_content_file_id(message->content.get());
      if (file_id.is_valid()) {
        if (G()->close_flag() && G()->parameters().use_message_db) {
          // do not send error, message will be re-sent
          return;
        }
        if (begins_with(error.message(), "FILE_PART_") && ends_with(error.message(), "_MISSING")) {
          on_send_message_file_part_missing(random_id, to_integer<int32>(error.message().substr(10)));
          return;
        }

        if (error.code() != 429 && error.code() < 500 && !G()->close_flag()) {
          td_->file_manager_->delete_partial_remote_location(file_id);
        }
      }
    }
  }

  on_send_message_fail(random_id, std::move(error));
}

void MessagesManager::on_send_secret_message_success(int64 random_id, MessageId message_id, int32 date,
                                                     tl_object_ptr<telegram_api::EncryptedFile> file_ptr,
                                                     Promise<> promise) {
  promise.set_value(Unit());  // TODO: set after message is saved

  FileId new_file_id;
  if (file_ptr != nullptr && file_ptr->get_id() == telegram_api::encryptedFile::ID) {
    auto file = move_tl_object_as<telegram_api::encryptedFile>(file_ptr);
    if (!DcId::is_valid(file->dc_id_)) {
      LOG(ERROR) << "Wrong dc_id = " << file->dc_id_ << " in file " << to_string(file);
    } else {
      DialogId owner_dialog_id;
      auto it = being_sent_messages_.find(random_id);
      if (it != being_sent_messages_.end()) {
        owner_dialog_id = it->second.get_dialog_id();
      }

      new_file_id = td_->file_manager_->register_remote(
          FullRemoteFileLocation(FileType::Encrypted, file->id_, file->access_hash_, DcId::internal(file->dc_id_)),
          FileLocationSource::FromServer, owner_dialog_id, 0, 0, "");
    }
  }

  on_send_message_success(random_id, message_id, date, new_file_id, "process send_secret_message_success");
}

void MessagesManager::delete_secret_messages(SecretChatId secret_chat_id, std::vector<int64> random_ids,
                                             Promise<> promise) {
  promise.set_value(Unit());  // TODO: set after event is saved
  DialogId dialog_id(secret_chat_id);
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(ERROR) << "Ignore delete secret messages in unknown " << dialog_id;
    return;
  }

  vector<MessageId> to_delete_message_ids;
  for (auto &random_id : random_ids) {
    auto message_id = get_message_id_by_random_id(d, random_id);
    if (!message_id.is_valid()) {
      continue;
    }
    const Message *m = get_message(d, message_id);
    CHECK(m != nullptr);
    if (!is_service_message_content(m->content->get_id())) {
      to_delete_message_ids.push_back(message_id);
    }
  }
  delete_dialog_messages_from_updates(dialog_id, to_delete_message_ids);
}

void MessagesManager::delete_secret_chat_history(SecretChatId secret_chat_id, MessageId last_message_id,
                                                 Promise<> promise) {
  promise.set_value(Unit());  // TODO: set after event is saved
  auto dialog_id = DialogId(secret_chat_id);
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(ERROR) << "Ignore delete secret chat history in unknown " << dialog_id;
    return;
  }

  // TODO: probably last_message_id is not needed
  delete_all_dialog_messages(d, false, true);
}

void MessagesManager::read_secret_chat_outbox(SecretChatId secret_chat_id, int32 up_to_date, int32 read_date) {
  if (!secret_chat_id.is_valid()) {
    LOG(ERROR) << "Receive read secret chat outbox in the invalid " << secret_chat_id;
    return;
  }
  auto dialog_id = DialogId(secret_chat_id);
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return;
  }
  // TODO: protect with logevent
  suffix_load_till_date(
      d, up_to_date,
      PromiseCreator::lambda([actor_id = actor_id(this), dialog_id, up_to_date, read_date](Result<Unit> result) {
        send_closure(actor_id, &MessagesManager::read_secret_chat_outbox_inner, dialog_id, up_to_date, read_date);
      }));
}

void MessagesManager::read_secret_chat_outbox_inner(DialogId dialog_id, int32 up_to_date, int32 read_date) {
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  auto end = MessagesConstIterator(d, MessageId::max());
  while (*end && (*end)->date > up_to_date) {
    --end;
  }
  if (!*end) {
    LOG(INFO) << "Ignore read_secret_chat_outbox in " << dialog_id << " at " << up_to_date
              << ": no messages with such date are known";
    return;
  }
  auto max_message_id = (*end)->message_id;
  read_history_outbox(dialog_id, max_message_id, read_date);
}

void MessagesManager::open_secret_message(SecretChatId secret_chat_id, int64 random_id, Promise<> promise) {
  promise.set_value(Unit());  // TODO: set after event is saved
  DialogId dialog_id(secret_chat_id);
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(ERROR) << "Ignore opening secret chat message in unknown " << dialog_id;
    return;
  }

  auto message_id = get_message_id_by_random_id(d, random_id);
  if (!message_id.is_valid()) {
    return;
  }
  Message *m = get_message(d, message_id);
  CHECK(m != nullptr);
  if (message_id.is_yet_unsent() || !m->is_outgoing) {
    LOG(ERROR) << "Peer has opened wrong " << message_id << " in " << dialog_id;
    return;
  }

  read_message_content(d, m, false, "open_secret_message");
}

void MessagesManager::on_get_secret_message(SecretChatId secret_chat_id, UserId user_id, MessageId message_id,
                                            int32 date, tl_object_ptr<telegram_api::encryptedFile> file,
                                            tl_object_ptr<secret_api::decryptedMessage> message, Promise<> promise) {
  LOG(DEBUG) << "On get " << to_string(message);
  CHECK(message != nullptr);
  CHECK(secret_chat_id.is_valid());
  CHECK(user_id.is_valid());
  CHECK(message_id.is_valid());
  CHECK(date > 0);

  auto pending_secret_message = make_unique<PendingSecretMessage>();
  pending_secret_message->success_promise = std::move(promise);
  MessageInfo &message_info = pending_secret_message->message_info;
  message_info.dialog_id = DialogId(secret_chat_id);
  message_info.message_id = message_id;
  message_info.sender_user_id = user_id;
  message_info.date = date;
  message_info.random_id = message->random_id_;
  message_info.ttl = message->ttl_;

  Dialog *d = get_dialog_force(message_info.dialog_id);
  if (d == nullptr) {
    LOG(ERROR) << "Ignore secret message in unknown " << message_info.dialog_id;
    pending_secret_message->success_promise.set_error(Status::Error(500, "Chat not found"));
    return;
  }

  pending_secret_message->load_data_multipromise.add_promise(Auto());
  auto lock_promise = pending_secret_message->load_data_multipromise.get_promise();

  int32 flags = MESSAGE_FLAG_HAS_UNREAD_CONTENT | MESSAGE_FLAG_HAS_FROM_ID;
  if ((message->flags_ & secret_api::decryptedMessage::REPLY_TO_RANDOM_ID_MASK) != 0) {
    message_info.reply_to_message_id =
        get_message_id_by_random_id(get_dialog(message_info.dialog_id), message->reply_to_random_id_);
    if (message_info.reply_to_message_id.is_valid()) {
      flags |= MESSAGE_FLAG_IS_REPLY;
    }
  }
  if ((message->flags_ & secret_api::decryptedMessage::ENTITIES_MASK) != 0) {
    flags |= MESSAGE_FLAG_HAS_ENTITIES;
  }
  if ((message->flags_ & secret_api::decryptedMessage::MEDIA_MASK) != 0) {
    flags |= MESSAGE_FLAG_HAS_MEDIA;
  }

  if (!clean_input_string(message->via_bot_name_)) {
    LOG(WARNING) << "Receive invalid bot username " << message->via_bot_name_;
    message->via_bot_name_.clear();
  }
  if ((message->flags_ & secret_api::decryptedMessage::VIA_BOT_NAME_MASK) != 0 && !message->via_bot_name_.empty()) {
    pending_secret_message->load_data_multipromise.add_promise(
        PromiseCreator::lambda([this, via_bot_name = message->via_bot_name_, &flags = message_info.flags,
                                &via_bot_user_id = message_info.via_bot_user_id](Unit) mutable {
          auto dialog_id = resolve_dialog_username(via_bot_name);
          if (dialog_id.is_valid() && dialog_id.get_type() == DialogType::User) {
            flags |= MESSAGE_FLAG_IS_SENT_VIA_BOT;
            via_bot_user_id = dialog_id.get_user_id();
          }
        }));
    search_public_dialog(message->via_bot_name_, false, pending_secret_message->load_data_multipromise.get_promise());
  }
  if ((message->flags_ & secret_api::decryptedMessage::GROUPED_ID_MASK) != 0 && message->grouped_id_ != 0) {
    message_info.media_album_id = message->grouped_id_;
    flags |= MESSAGE_FLAG_HAS_MEDIA_ALBUM_ID;
  }

  message_info.flags = flags;
  message_info.content = get_secret_message_content(
      std::move(message->message_), std::move(file), std::move(message->media_), std::move(message->entities_),
      message_info.dialog_id, pending_secret_message->load_data_multipromise);

  add_secret_message(std::move(pending_secret_message), std::move(lock_promise));
}

void MessagesManager::on_secret_chat_screenshot_taken(SecretChatId secret_chat_id, UserId user_id, MessageId message_id,
                                                      int32 date, int64 random_id, Promise<> promise) {
  LOG(DEBUG) << "On screenshot taken in " << secret_chat_id;
  CHECK(secret_chat_id.is_valid());
  CHECK(user_id.is_valid());
  CHECK(message_id.is_valid());
  CHECK(date > 0);

  auto pending_secret_message = make_unique<PendingSecretMessage>();
  pending_secret_message->success_promise = std::move(promise);
  MessageInfo &message_info = pending_secret_message->message_info;
  message_info.dialog_id = DialogId(secret_chat_id);
  message_info.message_id = message_id;
  message_info.sender_user_id = user_id;
  message_info.date = date;
  message_info.random_id = random_id;
  message_info.flags = MESSAGE_FLAG_HAS_FROM_ID;
  message_info.content = make_unique<MessageScreenshotTaken>();

  Dialog *d = get_dialog_force(message_info.dialog_id);
  if (d == nullptr) {
    LOG(ERROR) << "Ignore secret message in unknown " << message_info.dialog_id;
    pending_secret_message->success_promise.set_error(Status::Error(500, "Chat not found"));
    return;
  }

  add_secret_message(std::move(pending_secret_message));
}

void MessagesManager::on_secret_chat_ttl_changed(SecretChatId secret_chat_id, UserId user_id, MessageId message_id,
                                                 int32 date, int32 ttl, int64 random_id, Promise<> promise) {
  LOG(DEBUG) << "On ttl set in " << secret_chat_id << " to " << ttl;
  CHECK(secret_chat_id.is_valid());
  CHECK(user_id.is_valid());
  CHECK(message_id.is_valid());
  CHECK(date > 0);
  if (ttl < 0) {
    LOG(WARNING) << "Receive wrong ttl = " << ttl;
    promise.set_value(Unit());
    return;
  }

  auto pending_secret_message = make_unique<PendingSecretMessage>();
  pending_secret_message->success_promise = std::move(promise);
  MessageInfo &message_info = pending_secret_message->message_info;
  message_info.dialog_id = DialogId(secret_chat_id);
  message_info.message_id = message_id;
  message_info.sender_user_id = user_id;
  message_info.date = date;
  message_info.random_id = random_id;
  message_info.flags = MESSAGE_FLAG_HAS_FROM_ID;
  message_info.content = make_unique<MessageChatSetTtl>(ttl);

  Dialog *d = get_dialog_force(message_info.dialog_id);
  if (d == nullptr) {
    LOG(ERROR) << "Ignore secret message in unknown " << message_info.dialog_id;
    pending_secret_message->success_promise.set_error(Status::Error(500, "Chat not found"));
    return;
  }

  add_secret_message(std::move(pending_secret_message));
}

void MessagesManager::add_secret_message(unique_ptr<PendingSecretMessage> pending_secret_message,
                                         Promise<Unit> lock_promise) {
  auto &multipromise = pending_secret_message->load_data_multipromise;
  multipromise.set_ignore_errors(true);
  int64 token = pending_secret_messages_.add(std::move(pending_secret_message));

  multipromise.add_promise(PromiseCreator::lambda([token, actor_id = actor_id(this),
                                                   this](Result<Unit> result) mutable {
    if (result.is_ok()) {  // if we aren't closing
      this->pending_secret_messages_.finish(token, [actor_id](unique_ptr<PendingSecretMessage> pending_secret_message) {
        send_closure_later(actor_id, &MessagesManager::finish_add_secret_message, std::move(pending_secret_message));
      });
    }
  }));

  if (!lock_promise) {
    lock_promise = multipromise.get_promise();
  }
  lock_promise.set_value(Unit());
}

void MessagesManager::finish_add_secret_message(unique_ptr<PendingSecretMessage> pending_secret_message) {
  auto d = get_dialog(pending_secret_message->message_info.dialog_id);
  CHECK(d != nullptr);
  auto random_id = pending_secret_message->message_info.random_id;
  auto message_id = get_message_id_by_random_id(d, random_id);
  if (message_id.is_valid()) {
    if (message_id != pending_secret_message->message_info.message_id) {
      LOG(WARNING) << "Ignore duplicate " << pending_secret_message->message_info.message_id
                   << " received earlier with " << message_id << " and random_id " << random_id;
    }
  } else {
    on_get_message(std::move(pending_secret_message->message_info), true, false, true, true,
                   "finish add secret message");
  }
  pending_secret_message->success_promise.set_value(Unit());  // TODO: set after message is saved
}

void MessagesManager::fix_message_info_dialog_id(MessageInfo &message_info) const {
  if (message_info.dialog_id != DialogId(td_->contacts_manager_->get_my_id("fix_message_info_dialog_id"))) {
    return;
  }

  UserId sender_user_id = message_info.sender_user_id;
  if (!sender_user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid sender user id in private chat";
    return;
  }

  message_info.dialog_id = DialogId(sender_user_id);
  LOG_IF(ERROR, (message_info.flags & MESSAGE_FLAG_IS_OUT) != 0)
      << "Receive message out flag for incoming " << message_info.message_id << " in " << message_info.dialog_id;
}

MessagesManager::MessageInfo MessagesManager::parse_telegram_api_message(
    tl_object_ptr<telegram_api::Message> message_ptr, const char *source) const {
  LOG(DEBUG) << "Receive from " << source << " " << to_string(message_ptr);
  CHECK(message_ptr != nullptr) << source;
  int32 constructor_id = message_ptr->get_id();

  MessageInfo message_info;
  switch (constructor_id) {
    case telegram_api::messageEmpty::ID:
      break;
    case telegram_api::message::ID: {
      auto message = move_tl_object_as<telegram_api::message>(message_ptr);

      message_info.dialog_id = DialogId(message->to_id_);
      message_info.message_id = MessageId(ServerMessageId(message->id_));
      if (message->flags_ & MESSAGE_FLAG_HAS_FROM_ID) {
        message_info.sender_user_id = UserId(message->from_id_);
      }
      message_info.date = message->date_;
      message_info.forward_header = std::move(message->fwd_from_);
      message_info.reply_to_message_id = MessageId(ServerMessageId(
          message->flags_ & MESSAGE_FLAG_IS_REPLY ? message->reply_to_msg_id_ : 0));  // TODO zero init in fetch
      if (message->flags_ & MESSAGE_FLAG_IS_SENT_VIA_BOT) {
        message_info.via_bot_user_id = UserId(message->via_bot_id_);
        if (!message_info.via_bot_user_id.is_valid()) {
          LOG(ERROR) << "Receive invalid " << message_info.via_bot_user_id << " from " << source;
          message_info.via_bot_user_id = UserId();
        }
      }
      if (message->flags_ & MESSAGE_FLAG_HAS_VIEWS) {
        message_info.views = message->views_;
      }
      if (message->flags_ & MESSAGE_FLAG_HAS_EDIT_DATE) {
        message_info.edit_date = message->edit_date_;
      }
      if (message->flags_ & MESSAGE_FLAG_HAS_MEDIA_ALBUM_ID) {
        message_info.media_album_id = message->grouped_id_;
      }
      message_info.flags = message->flags_;
      fix_message_info_dialog_id(message_info);
      bool is_content_read = (message->flags_ & MESSAGE_FLAG_HAS_UNREAD_CONTENT) == 0;
      if (is_message_auto_read(message_info.dialog_id, (message->flags_ & MESSAGE_FLAG_IS_OUT) != 0, true)) {
        is_content_read = true;
      }
      message_info.content = get_message_content(
          get_message_text(std::move(message->message_), std::move(message->entities_),
                           message_info.forward_header ? message_info.forward_header->date_ : message_info.date),
          std::move(message->media_), message_info.dialog_id, is_content_read, message_info.via_bot_user_id,
          &message_info.ttl);
      message_info.reply_markup =
          message->flags_ & MESSAGE_FLAG_HAS_REPLY_MARKUP ? std::move(message->reply_markup_) : nullptr;
      message_info.author_signature = std::move(message->post_author_);
      break;
    }
    case telegram_api::messageService::ID: {
      auto message = move_tl_object_as<telegram_api::messageService>(message_ptr);

      message_info.dialog_id = DialogId(message->to_id_);
      message_info.message_id = MessageId(ServerMessageId(message->id_));
      if (message->flags_ & MESSAGE_FLAG_HAS_FROM_ID) {
        message_info.sender_user_id = UserId(message->from_id_);
      }
      message_info.date = message->date_;
      message_info.flags = message->flags_;
      fix_message_info_dialog_id(message_info);
      MessageId reply_to_message_id = MessageId(ServerMessageId(
          message->flags_ & MESSAGE_FLAG_IS_REPLY ? message->reply_to_msg_id_ : 0));  // TODO zero init in fetch
      message_info.content =
          get_message_action_content(std::move(message->action_), message_info.dialog_id, reply_to_message_id);
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
  return message_info;
}

std::pair<DialogId, unique_ptr<MessagesManager::Message>> MessagesManager::create_message(MessageInfo &&message_info,
                                                                                          bool is_channel_message) {
  DialogId dialog_id = message_info.dialog_id;
  MessageId message_id = message_info.message_id;
  if (!message_id.is_valid() || !dialog_id.is_valid()) {
    if (message_id != MessageId() || dialog_id != DialogId()) {
      LOG(ERROR) << "Receive " << message_id << " in " << dialog_id;
    }
    return {DialogId(), nullptr};
  }
  if (message_id.is_yet_unsent()) {
    LOG(ERROR) << "Receive " << message_id;
    return {DialogId(), nullptr};
  }

  CHECK(message_info.content != nullptr);

  UserId sender_user_id = message_info.sender_user_id;
  if (!sender_user_id.is_valid()) {
    if (!is_broadcast_channel(dialog_id)) {
      LOG(ERROR) << "Invalid " << sender_user_id << " specified to be a sender of the " << message_id << " in "
                 << dialog_id;
      return {DialogId(), nullptr};
    }

    if (sender_user_id != UserId()) {
      LOG(ERROR) << "Receive invalid " << sender_user_id;
      sender_user_id = UserId();
    }
  }

  auto dialog_type = dialog_id.get_type();
  LOG_IF(ERROR, is_channel_message && dialog_type != DialogType::Channel)
      << "is_channel_message is true for message received in the " << dialog_id;

  int32 flags = message_info.flags;
  if (flags & ~(MESSAGE_FLAG_IS_OUT | MESSAGE_FLAG_IS_FORWARDED | MESSAGE_FLAG_IS_REPLY | MESSAGE_FLAG_HAS_MENTION |
                MESSAGE_FLAG_HAS_UNREAD_CONTENT | MESSAGE_FLAG_HAS_REPLY_MARKUP | MESSAGE_FLAG_HAS_ENTITIES |
                MESSAGE_FLAG_HAS_FROM_ID | MESSAGE_FLAG_HAS_MEDIA | MESSAGE_FLAG_HAS_VIEWS |
                MESSAGE_FLAG_IS_SENT_VIA_BOT | MESSAGE_FLAG_IS_SILENT | MESSAGE_FLAG_IS_POST |
                MESSAGE_FLAG_HAS_EDIT_DATE | MESSAGE_FLAG_HAS_AUTHOR_SIGNATURE | MESSAGE_FLAG_HAS_MEDIA_ALBUM_ID)) {
    LOG(ERROR) << "Unsupported message flags = " << flags << " received";
  }

  bool is_outgoing = (flags & MESSAGE_FLAG_IS_OUT) != 0;
  bool is_silent = (flags & MESSAGE_FLAG_IS_SILENT) != 0;
  bool is_channel_post = (flags & MESSAGE_FLAG_IS_POST) != 0;

  UserId my_id = td_->contacts_manager_->get_my_id("create_message");
  DialogId my_dialog_id = DialogId(my_id);
  if (dialog_id == my_dialog_id) {
    // dialog_id should be already fixed
    CHECK(sender_user_id == my_id);
  }

  if (sender_user_id.is_valid() && (sender_user_id == my_id && dialog_id != my_dialog_id) != is_outgoing) {
    //    if (content->get_id() != MessageChatAddUser::ID) {  // TODO: we have wrong flags for invites via links
    LOG(ERROR) << "Receive wrong message out flag: me is " << my_id << ", message is from " << sender_user_id
               << ", flags = " << flags << " for " << message_id << " in " << dialog_id;
    //    }
  }

  MessageId reply_to_message_id = message_info.reply_to_message_id;
  if (reply_to_message_id != MessageId() &&
      (!reply_to_message_id.is_valid() || reply_to_message_id.get() >= message_id.get())) {
    LOG(ERROR) << "Receive reply to wrong " << reply_to_message_id << " in " << message_id;
    reply_to_message_id = MessageId();
  }

  UserId via_bot_user_id = message_info.via_bot_user_id;
  if (!via_bot_user_id.is_valid()) {
    via_bot_user_id = UserId();
  }

  int32 date = message_info.date;
  if (date <= 0) {
    LOG(ERROR) << "Wrong date = " << date << " received in " << message_id << " in " << dialog_id;
    date = 1;
  }

  int32 edit_date = message_info.edit_date;
  if (edit_date < 0) {
    LOG(ERROR) << "Wrong edit_date = " << edit_date << " received in " << message_id << " in " << dialog_id;
    edit_date = 0;
  }

  int32 ttl = message_info.ttl;
  bool is_content_secret =
      is_secret_message_content(ttl, message_info.content->get_id());  // should be calculated before TTL is adjusted
  if (ttl < 0) {
    LOG(ERROR) << "Wrong ttl = " << ttl << " received in " << message_id << " in " << dialog_id;
    ttl = 0;
  } else if (ttl > 0) {
    ttl = max(ttl, get_message_content_duration(message_info.content.get()) + 1);
  }

  int32 views = message_info.views;
  if (views < 0) {
    LOG(ERROR) << "Wrong views = " << views << " received in " << message_id << " in " << dialog_id;
    views = 0;
  }

  LOG(INFO) << "Receive " << message_id << " in " << dialog_id << " from " << sender_user_id;

  auto message = make_unique<Message>();
  message->random_y = get_random_y(message_id);
  message->message_id = message_id;
  message->sender_user_id = sender_user_id;
  message->date = date;
  message->ttl = ttl;
  message->edit_date = edit_date;
  message->random_id = message_info.random_id;
  message->forward_info = get_message_forward_info(std::move(message_info.forward_header));
  message->reply_to_message_id = reply_to_message_id;
  message->via_bot_user_id = via_bot_user_id;
  message->author_signature = std::move(message_info.author_signature);
  message->is_outgoing = is_outgoing;
  message->is_channel_post = is_channel_post;
  message->contains_mention =
      !is_outgoing && dialog_type != DialogType::User && (flags & MESSAGE_FLAG_HAS_MENTION) != 0;
  message->contains_unread_mention =
      message_id.is_server() && message->contains_mention && (flags & MESSAGE_FLAG_HAS_UNREAD_CONTENT) != 0 &&
      (dialog_type == DialogType::Chat || (dialog_type == DialogType::Channel && !is_broadcast_channel(dialog_id)));
  message->disable_notification = is_silent;
  message->is_content_secret = is_content_secret;
  message->views = views;
  message->content = std::move(message_info.content);
  message->reply_markup = get_reply_markup(std::move(message_info.reply_markup), td_->auth_manager_->is_bot(), false,
                                           message->contains_mention || dialog_id.get_type() == DialogType::User);

  auto content_id = message->content->get_id();
  if (content_id == MessageExpiredPhoto::ID || content_id == MessageExpiredVideo::ID) {
    CHECK(message->ttl == 0);  // ttl is ignored/set to 0 if the message has already been expired
    if (message->reply_markup != nullptr) {
      if (message->reply_markup->type != ReplyMarkup::Type::InlineKeyboard) {
        message->had_reply_markup = true;
      }
      message->reply_markup = nullptr;
    }
    message->reply_to_message_id = MessageId();
  }

  if (message_info.media_album_id != 0) {
    if (!is_allowed_media_group_content(content_id)) {
      LOG(ERROR) << "Receive media group id " << message_info.media_album_id << " in " << message_id << " from "
                 << dialog_id << " with content "
                 << oneline(to_string(
                        get_message_content_object(message->content.get(), message->date, is_content_secret)));
    } else {
      message->media_album_id = message_info.media_album_id;
    }
  }

  return {dialog_id, std::move(message)};
}

FullMessageId MessagesManager::on_get_message(tl_object_ptr<telegram_api::Message> message_ptr, bool from_update,
                                              bool is_channel_message, bool have_previous, bool have_next,
                                              const char *source) {
  return on_get_message(parse_telegram_api_message(std::move(message_ptr), source), from_update, is_channel_message,
                        have_previous, have_next, source);
}

FullMessageId MessagesManager::on_get_message(MessageInfo &&message_info, bool from_update, bool is_channel_message,
                                              bool have_previous, bool have_next, const char *source) {
  DialogId dialog_id;
  unique_ptr<Message> new_message;
  std::tie(dialog_id, new_message) = create_message(std::move(message_info), is_channel_message);
  if (new_message == nullptr) {
    return FullMessageId();
  }
  MessageId message_id = new_message->message_id;

  DialogId my_dialog_id = DialogId(td_->contacts_manager_->get_my_id("on_get_message"));

  new_message->have_previous = have_previous;
  new_message->have_next = have_next;

  bool need_update = from_update;
  bool need_update_dialog_pos = false;

  FullMessageId full_message_id(dialog_id, message_id);
  auto it = update_message_ids_.find(full_message_id);
  if (it != update_message_ids_.end()) {
    Dialog *d = get_dialog(dialog_id);
    CHECK(d != nullptr);

    if (!from_update) {
      LOG_IF(ERROR, message_id.get() <= d->last_new_message_id.get())
          << "New " << message_id << " has id less than last_new_message_id = " << d->last_new_message_id;
      LOG(ERROR) << "Ignore " << it->second << " received not through update from " << source << ": "
                 << oneline(to_string(get_message_object(dialog_id, new_message.get())));  // TODO move to INFO
      dump_debug_message_op(d, 3);                                                         // TODO remove
      if (dialog_id.get_type() == DialogType::Channel && have_input_peer(dialog_id, AccessRights::Read)) {
        channel_get_difference_retry_timeout_.add_timeout_in(dialog_id.get(), 0.001);
      }
      return FullMessageId();
    }

    MessageId old_message_id = it->second;

    update_message_ids_.erase(it);

    if (!new_message->is_outgoing && dialog_id != my_dialog_id) {
      // sent message is not from me
      LOG(ERROR) << "Sent in " << dialog_id << " " << message_id << " is sent by " << new_message->sender_user_id;
      return FullMessageId();
    }

    unique_ptr<Message> old_message =
        delete_message(d, old_message_id, false, &need_update_dialog_pos, "add sent message");
    if (old_message == nullptr) {
      // message has already been deleted by the user or sent to inaccessible channel
      // don't need to send update to the user, because the message has already been deleted
      LOG(INFO) << "Delete already deleted sent " << new_message->message_id << " from server";
      delete_messages_from_server(dialog_id, {new_message->message_id}, true, 0, Auto());
      return FullMessageId();
    }

    need_update = false;

    new_message->message_id = old_message_id;
    new_message->random_y = get_random_y(new_message->message_id);
    new_message->have_previous = false;
    new_message->have_next = false;
    update_message(d, old_message, std::move(new_message), true, &need_update_dialog_pos);
    new_message = std::move(old_message);

    new_message->message_id = message_id;
    new_message->random_y = get_random_y(new_message->message_id);
    send_update_message_send_succeeded(d, old_message_id, new_message.get());

    try_add_active_live_location(dialog_id, new_message.get());

    new_message->have_previous = true;
    new_message->have_next = true;
  }

  Message *m = add_message_to_dialog(dialog_id, std::move(new_message), from_update, &need_update,
                                     &need_update_dialog_pos, source);
  Dialog *d = get_dialog(dialog_id);
  if (m == nullptr) {
    if (need_update_dialog_pos && d != nullptr) {
      send_update_chat_last_message(d, "on_get_message");
    }

    return FullMessageId();
  }

  CHECK(d != nullptr);

  auto pcc_it = pending_created_dialogs_.find(dialog_id);
  if (from_update && pcc_it != pending_created_dialogs_.end()) {
    pcc_it->second.set_value(Unit());

    pending_created_dialogs_.erase(pcc_it);
  }

  if (need_update) {
    send_update_new_message(d, m);
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    auto p = delete_message(d, message_id, false, &need_update_dialog_pos, "get a message in inaccessible chat");
    CHECK(p.get() == m);
    // CHECK(d->messages == nullptr);
    send_update_delete_messages(dialog_id, {message_id.get()}, false, false);
    // don't need to update dialog pos
    return FullMessageId();
  }

  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "on_get_message");
  }

  if (need_update && m->reply_markup != nullptr && m->reply_markup->type != ReplyMarkup::Type::InlineKeyboard &&
      m->reply_markup->is_personal && !td_->auth_manager_->is_bot()) {
    set_dialog_reply_markup(d, message_id);
  }

  return FullMessageId(dialog_id, message_id);
}

void MessagesManager::set_dialog_last_message_id(Dialog *d, MessageId last_message_id, const char *source) {
  LOG(INFO) << "Set " << d->dialog_id << " last message to " << last_message_id << " from " << source;
  d->last_message_id = last_message_id;

  if (last_message_id.is_valid() && d->delete_last_message_date != 0) {
    d->delete_last_message_date = 0;
    d->deleted_last_message_id = MessageId();
    d->is_last_message_deleted_locally = false;
    on_dialog_updated(d->dialog_id, "update_delete_last_message_date");
  }
}

void MessagesManager::set_dialog_first_database_message_id(Dialog *d, MessageId first_database_message_id,
                                                           const char *source) {
  LOG(INFO) << "Set " << d->dialog_id << " first database message to " << first_database_message_id << " from "
            << source;
  d->first_database_message_id = first_database_message_id;
}

void MessagesManager::set_dialog_last_database_message_id(Dialog *d, MessageId last_database_message_id,
                                                          const char *source) {
  LOG(INFO) << "Set " << d->dialog_id << " last database message to " << last_database_message_id << " from " << source;
  d->last_database_message_id = last_database_message_id;
}

void MessagesManager::set_dialog_last_new_message_id(Dialog *d, MessageId last_new_message_id, const char *source) {
  CHECK(last_new_message_id.get() > d->last_new_message_id.get());
  CHECK(d->dialog_id.get_type() == DialogType::SecretChat || last_new_message_id.is_server());
  if (!d->last_new_message_id.is_valid()) {
    delete_all_dialog_messages_from_database(d->dialog_id, MessageId::max(), "set_dialog_last_new_message_id");
    set_dialog_first_database_message_id(d, MessageId(), "set_dialog_last_new_message_id");
    set_dialog_last_database_message_id(d, MessageId(), "set_dialog_last_new_message_id");
    if (d->dialog_id.get_type() != DialogType::SecretChat) {
      d->have_full_history = false;
    }
    for (auto &first_message_id : d->first_database_message_id_by_index) {
      first_message_id = last_new_message_id;
    }

    MessagesConstIterator it(d, MessageId::max());
    vector<MessageId> to_delete_message_ids;
    while (*it != nullptr) {
      auto message_id = (*it)->message_id;
      if (message_id.get() <= last_new_message_id.get()) {
        break;
      }
      if (!message_id.is_yet_unsent()) {
        to_delete_message_ids.push_back(message_id);
      }
      --it;
    }
    if (!to_delete_message_ids.empty()) {
      LOG(ERROR) << "Delete " << format::as_array(to_delete_message_ids) << " because of received last new "
                 << last_new_message_id << " in " << d->dialog_id;

      vector<int64> deleted_message_ids;
      bool need_update_dialog_pos = false;
      for (auto message_id : to_delete_message_ids) {
        if (delete_message(d, message_id, false, &need_update_dialog_pos, "set_dialog_last_new_message_id") !=
            nullptr) {
          deleted_message_ids.push_back(message_id.get());
        }
      }
      if (need_update_dialog_pos) {
        send_update_chat_last_message(d, "set_dialog_last_new_message_id");
      }
      send_update_delete_messages(d->dialog_id, std::move(deleted_message_ids), false, false);
    }

    auto last_new_message = get_message(d, last_new_message_id);
    if (last_new_message != nullptr) {
      add_message_to_database(d, last_new_message, "set_dialog_last_new_message_id");
      set_dialog_first_database_message_id(d, last_new_message_id, "set_dialog_last_new_message_id");
      set_dialog_last_database_message_id(d, last_new_message_id, "set_dialog_last_new_message_id");
      try_restore_dialog_reply_markup(d, last_new_message);
    }
  }

  LOG(INFO) << "Set " << d->dialog_id << " last new message to " << last_new_message_id << " from " << source;
  d->last_new_message_id = last_new_message_id;
  on_dialog_updated(d->dialog_id, source);
}

void MessagesManager::set_dialog_last_clear_history_date(Dialog *d, int32 date, MessageId last_clear_history_message_id,
                                                         const char *source) {
  LOG(INFO) << "Set " << d->dialog_id << " last clear history date to " << date << " of "
            << last_clear_history_message_id << " from " << source;
  if (d->last_clear_history_message_id.is_valid()) {
    switch (d->dialog_id.get_type()) {
      case DialogType::User:
      case DialogType::Chat:
        last_clear_history_message_id_to_dialog_id_.erase(d->last_clear_history_message_id);
        break;
      case DialogType::Channel:
      case DialogType::SecretChat:
        // nothing to do
        break;
      case DialogType::None:
      default:
        UNREACHABLE();
    }
  }

  d->last_clear_history_date = date;
  d->last_clear_history_message_id = last_clear_history_message_id;

  if (d->last_clear_history_message_id.is_valid()) {
    switch (d->dialog_id.get_type()) {
      case DialogType::User:
      case DialogType::Chat:
        last_clear_history_message_id_to_dialog_id_[d->last_clear_history_message_id] = d->dialog_id;
        break;
      case DialogType::Channel:
      case DialogType::SecretChat:
        // nothing to do
        break;
      case DialogType::None:
      default:
        UNREACHABLE();
    }
  }
}

void MessagesManager::set_dialog_is_empty(Dialog *d, const char *source) {
  LOG(INFO) << "Set " << d->dialog_id << " is_empty to true from " << source;
  d->is_empty = true;

  if (d->delete_last_message_date != 0) {
    if (d->is_last_message_deleted_locally && d->last_clear_history_date == 0) {
      set_dialog_last_clear_history_date(d, d->delete_last_message_date, d->deleted_last_message_id,
                                         "set_dialog_is_empty");
    }
    d->delete_last_message_date = 0;
    d->deleted_last_message_id = MessageId();
    d->is_last_message_deleted_locally = false;

    on_dialog_updated(d->dialog_id, "set_dialog_is_empty");
  }

  update_dialog_pos(d, false, source);
}

void MessagesManager::set_dialog_is_pinned(DialogId dialog_id, bool is_pinned) {
  Dialog *d = get_dialog(dialog_id);
  set_dialog_is_pinned(d, is_pinned);
  update_dialog_pos(d, false, "set_dialog_is_pinned");
}

void MessagesManager::set_dialog_is_pinned(Dialog *d, bool is_pinned) {
  CHECK(d != nullptr);
  bool was_pinned = d->pinned_order != DEFAULT_ORDER;
  d->pinned_order = is_pinned ? get_next_pinned_dialog_order() : DEFAULT_ORDER;
  on_dialog_updated(d->dialog_id, "set_dialog_is_pinned");

  if (is_pinned != was_pinned) {
    LOG(INFO) << "Set " << d->dialog_id << " is pinned to " << is_pinned;
    CHECK(d == get_dialog(d->dialog_id)) << "Wrong " << d->dialog_id << " in set_dialog_is_pinned";
    update_dialog_pos(d, false, "set_dialog_is_pinned", false);
    DialogDate dialog_date(d->order, d->dialog_id);
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateChatIsPinned>(d->dialog_id.get(), is_pinned,
                                                            dialog_date <= last_dialog_date_ ? d->order : 0));
  }
}

void MessagesManager::set_dialog_reply_markup(Dialog *d, MessageId message_id) {
  if (d->reply_markup_message_id != message_id) {
    on_dialog_updated(d->dialog_id, "set_dialog_reply_markup");
  }

  d->need_restore_reply_markup = false;

  if (d->reply_markup_message_id.is_valid() || message_id.is_valid()) {
    CHECK(d == get_dialog(d->dialog_id)) << "Wrong " << d->dialog_id << " in set_dialog_reply_markup";
    d->reply_markup_message_id = message_id;
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateChatReplyMarkup>(d->dialog_id.get(), message_id.get()));
  }
}

void MessagesManager::try_restore_dialog_reply_markup(Dialog *d, const Message *m) {
  if (!d->need_restore_reply_markup || td_->auth_manager_->is_bot()) {
    return;
  }

  if (m->had_reply_markup) {
    LOG(INFO) << "Restore deleted reply markup in " << d->dialog_id;
    set_dialog_reply_markup(d, MessageId());
  } else if (m->reply_markup != nullptr && m->reply_markup->type != ReplyMarkup::Type::InlineKeyboard &&
             m->reply_markup->is_personal) {
    LOG(INFO) << "Restore reply markup in " << d->dialog_id << " to " << m->message_id;
    set_dialog_reply_markup(d, m->message_id);
  }
}

// TODO this function needs to be merged with on_send_message_success
void MessagesManager::on_update_sent_text_message(int64 random_id,
                                                  tl_object_ptr<telegram_api::MessageMedia> message_media,
                                                  vector<tl_object_ptr<telegram_api::MessageEntity>> &&entities) {
  CHECK(message_media != nullptr);
  int32 message_media_id = message_media->get_id();
  LOG_IF(ERROR, message_media_id != telegram_api::messageMediaWebPage::ID &&
                    message_media_id != telegram_api::messageMediaEmpty::ID)
      << "Receive non web-page media for text message: " << oneline(to_string(message_media));

  auto it = being_sent_messages_.find(random_id);
  if (it == being_sent_messages_.end()) {
    // result of sending message has already been received through getDifference
    return;
  }

  auto dialog_id = it->second.get_dialog_id();
  auto m = get_message_force(it->second);
  if (m == nullptr) {
    // message has already been deleted
    return;
  }

  if (m->content->get_id() != MessageText::ID) {
    LOG(ERROR) << "Text message content has been already changed to " << m->content->get_id();
    return;
  }
  auto message_text = static_cast<const MessageText *>(m->content.get());

  auto new_content = get_message_content(
      get_message_text(message_text->text.text, std::move(entities), m->forward_info ? m->forward_info->date : m->date),
      std::move(message_media), dialog_id, true /*likely ignored*/, UserId() /*likely ignored*/, nullptr /*ignored*/);
  if (new_content->get_id() != MessageText::ID) {
    LOG(ERROR) << "Text message content has changed to " << new_content->get_id();
    return;
  }
  auto new_message_text = static_cast<const MessageText *>(new_content.get());

  bool need_update = false;
  bool is_content_changed = false;

  if (message_text->text.entities != new_message_text->text.entities) {
    is_content_changed = true;
    need_update = true;
  }
  if (message_text->web_page_id != new_message_text->web_page_id) {
    LOG(INFO) << "Old: " << message_text->web_page_id << ", new: " << new_message_text->web_page_id;
    is_content_changed = true;
    need_update |= td_->web_pages_manager_->have_web_page(message_text->web_page_id) ||
                   td_->web_pages_manager_->have_web_page(new_message_text->web_page_id);
  }

  if (is_content_changed) {
    m->content = std::move(new_content);
    m->is_content_secret = is_secret_message_content(m->ttl, m->content->get_id());
  }
  if (need_update) {
    send_update_message_content(dialog_id, m->message_id, m->content.get(), m->date, m->is_content_secret,
                                "on_update_sent_text_message");
  }
}

void MessagesManager::on_update_message_web_page(FullMessageId full_message_id, bool have_web_page) {
  waiting_for_web_page_messages_.erase(full_message_id);
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog(dialog_id);
  if (d == nullptr) {
    LOG(INFO) << "Can't find " << dialog_id;
    // dialog can be not yet added
    return;
  }
  auto message_id = full_message_id.get_message_id();
  Message *message = get_message(d, message_id);
  if (message == nullptr) {
    // message can be already deleted
    return;
  }
  CHECK(message->date > 0);
  auto content_type = message->content->get_id();
  CHECK(content_type == MessageText::ID);
  auto content = static_cast<MessageText *>(message->content.get());
  if (!content->web_page_id.is_valid()) {
    // webpage has already been received as empty
    LOG_IF(ERROR, have_web_page) << "Receive earlier not received web page";
    return;
  }

  if (!have_web_page) {
    content->web_page_id = WebPageId();
    // don't need to send an update

    on_message_changed(d, message, "on_update_message_web_page");
    return;
  }

  send_update_message_content(dialog_id, message_id, content, message->date, message->is_content_secret,
                              "on_update_message_web_page");
}

void MessagesManager::on_get_dialogs(vector<tl_object_ptr<telegram_api::dialog>> &&dialogs, int32 total_count,
                                     vector<tl_object_ptr<telegram_api::Message>> &&messages, Promise<Unit> &&promise) {
  if (td_->updates_manager_->running_get_difference()) {
    LOG(INFO) << "Postpone result of getDialogs";
    pending_on_get_dialogs_.push_back(
        PendingOnGetDialogs{std::move(dialogs), total_count, std::move(messages), std::move(promise)});
    return;
  }
  bool from_dialog_list = total_count >= 0;
  bool from_get_dialog = total_count == -1;
  bool from_pinned_dialog_list = total_count == -2;

  if (from_get_dialog && dialogs.size() == 1) {
    DialogId dialog_id(dialogs[0]->peer_);
    if (running_get_channel_difference(dialog_id)) {
      LOG(INFO) << "Postpone result of channels getDialogs for " << dialog_id;
      pending_channel_on_get_dialogs_.emplace(
          dialog_id, PendingOnGetDialogs{std::move(dialogs), total_count, std::move(messages), std::move(promise)});
      return;
    }
  }

  LOG(INFO) << "Receive " << dialogs.size() << " dialogs out of " << total_count << " in result of GetDialogsQuery";
  std::unordered_map<FullMessageId, DialogDate, FullMessageIdHash> full_message_id_to_dialog_date;
  std::unordered_map<FullMessageId, tl_object_ptr<telegram_api::Message>, FullMessageIdHash> full_message_id_to_message;
  for (auto &message : messages) {
    auto full_message_id = get_full_message_id(message);
    if (from_dialog_list) {
      auto message_date = get_message_date(message);
      int64 order = get_dialog_order(full_message_id.get_message_id(), message_date);
      full_message_id_to_dialog_date.emplace(full_message_id, DialogDate(order, full_message_id.get_dialog_id()));
    }
    full_message_id_to_message[full_message_id] = std::move(message);
  }

  DialogDate max_dialog_date = MIN_DIALOG_DATE;
  for (auto &dialog : dialogs) {
    //    LOG(INFO) << to_string(dialog);
    DialogId dialog_id(dialog->peer_);
    bool has_pts = (dialog->flags_ & DIALOG_FLAG_HAS_PTS) != 0;

    if (!dialog_id.is_valid()) {
      LOG(ERROR) << "Receive wrong " << dialog_id;
      return promise.set_error(Status::Error(500, "Wrong query result returned: receive wrong chat identifier"));
    }
    switch (dialog_id.get_type()) {
      case DialogType::User:
      case DialogType::Chat:
        if (has_pts) {
          LOG(ERROR) << "Receive user or group " << dialog_id << " with pts";
          return promise.set_error(
              Status::Error(500, "Wrong query result returned: receive user or basic group chat with pts"));
        }
        break;
      case DialogType::Channel:
        if (!has_pts) {
          LOG(ERROR) << "Receive channel " << dialog_id << "without pts";
          return promise.set_error(
              Status::Error(500, "Wrong query result returned: receive supergroup chat without pts"));
        }
        break;
      case DialogType::SecretChat:
      case DialogType::None:
      default:
        UNREACHABLE();
        return promise.set_error(Status::Error(500, "UNREACHABLE"));
    }

    if (from_dialog_list) {
      MessageId last_message_id(ServerMessageId(dialog->top_message_));
      if (last_message_id.is_valid()) {
        FullMessageId full_message_id(dialog_id, last_message_id);
        auto it = full_message_id_to_dialog_date.find(full_message_id);
        if (it == full_message_id_to_dialog_date.end()) {
          // can happen for bots, TODO disable getChats for bots
          LOG(ERROR) << "Last " << last_message_id << " in " << dialog_id << " not found";
          return promise.set_error(Status::Error(500, "Wrong query result returned: last message not found"));
        }
        DialogDate dialog_date = it->second;
        CHECK(dialog_date.get_dialog_id() == dialog_id);

        if (max_dialog_date < dialog_date) {
          max_dialog_date = dialog_date;
        }
      } else {
        LOG(ERROR) << "Receive " << last_message_id << " as last chat message";
        continue;
      }
    }
  }

  if (from_dialog_list) {
    if (dialogs.empty()) {
      // if there is no more dialogs on the server
      max_dialog_date = MAX_DIALOG_DATE;
    }
    if (last_server_dialog_date_ < max_dialog_date) {
      last_server_dialog_date_ = max_dialog_date;
      update_last_dialog_date();
    } else {
      LOG(ERROR) << "Last server dialog date didn't increased";
    }
  }
  if (from_pinned_dialog_list) {
    max_dialog_date = DialogDate(get_dialog_order(MessageId(), MIN_PINNED_DIALOG_DATE - 1), DialogId());
    if (last_server_dialog_date_ < max_dialog_date) {
      last_server_dialog_date_ = max_dialog_date;
      update_last_dialog_date();
    }
  }

  vector<DialogId> added_dialog_ids;
  for (auto &dialog : dialogs) {
    MessageId last_message_id(ServerMessageId(dialog->top_message_));
    if (!last_message_id.is_valid() && from_dialog_list) {
      // skip dialogs without messages
      continue;
    }

    DialogId dialog_id(dialog->peer_);
    added_dialog_ids.push_back(dialog_id);
    Dialog *d = get_dialog_force(dialog_id);
    bool need_update_dialog_pos = false;
    if (d == nullptr) {
      d = add_dialog(dialog_id);
      need_update_dialog_pos = true;
    } else {
      LOG(INFO) << "Receive already created " << dialog_id;
      CHECK(d->dialog_id == dialog_id);
    }
    bool is_new = d->last_new_message_id == MessageId();

    on_update_notify_settings(dialog_id.get(), std::move(dialog->notify_settings_));

    if (dialog->unread_count_ < 0) {
      LOG(ERROR) << "Receive " << dialog->unread_count_ << " as number of unread messages in " << dialog_id;
      dialog->unread_count_ = 0;
    }
    MessageId read_inbox_max_message_id = MessageId(ServerMessageId(dialog->read_inbox_max_id_));
    if (!read_inbox_max_message_id.is_valid() && read_inbox_max_message_id != MessageId()) {
      LOG(ERROR) << "Receive " << read_inbox_max_message_id << " as last read inbox message in " << dialog_id;
      read_inbox_max_message_id = MessageId();
    }
    MessageId read_outbox_max_message_id = MessageId(ServerMessageId(dialog->read_outbox_max_id_));
    if (!read_outbox_max_message_id.is_valid() && read_outbox_max_message_id != MessageId()) {
      LOG(ERROR) << "Receive " << read_outbox_max_message_id << " as last read outbox message in " << dialog_id;
      read_outbox_max_message_id = MessageId();
    }
    if (dialog->unread_mentions_count_ < 0) {
      LOG(ERROR) << "Receive " << dialog->unread_mentions_count_ << " as number of unread mention messages in "
                 << dialog_id;
      dialog->unread_mentions_count_ = 0;
    }

    need_update_dialog_pos |= update_dialog_draft_message(
        d, get_draft_message(td_->contacts_manager_.get(), std::move(dialog->draft_)), true, false);
    if (is_new) {
      bool has_pts = (dialog->flags_ & DIALOG_FLAG_HAS_PTS) != 0;
      if (last_message_id.is_valid()) {
        FullMessageId full_message_id(dialog_id, last_message_id);
        auto added_full_message_id = on_get_message(std::move(full_message_id_to_message[full_message_id]), false,
                                                    has_pts, false, false, "get chats");
        CHECK(d->last_new_message_id == MessageId());
        set_dialog_last_new_message_id(d, full_message_id.get_message_id(), "on_get_dialogs");
        if (d->last_new_message_id.get() > d->last_message_id.get() &&
            added_full_message_id.get_message_id().is_valid()) {
          CHECK(added_full_message_id.get_message_id() == d->last_new_message_id);
          set_dialog_last_message_id(d, d->last_new_message_id, "on_get_dialogs");
          send_update_chat_last_message(d, "on_get_dialogs");
        }
      }

      if (has_pts && !running_get_channel_difference(dialog_id)) {
        int32 channel_pts = dialog->pts_;
        LOG_IF(ERROR, channel_pts < d->pts) << "In new " << dialog_id << " pts = " << d->pts
                                            << ", but pts = " << channel_pts << " received in GetChats";
        set_channel_pts(d, channel_pts, "get channel");
      }
    }
    bool is_pinned = (dialog->flags_ & DIALOG_FLAG_IS_PINNED) != 0;
    bool was_pinned = d->pinned_order != DEFAULT_ORDER;
    if (is_pinned != was_pinned) {
      set_dialog_is_pinned(d, is_pinned);
      need_update_dialog_pos = false;
    }

    if (need_update_dialog_pos) {
      update_dialog_pos(d, false, "on_get_dialogs");
    }

    if (!G()->parameters().use_message_db || is_new ||
        (!d->is_last_read_inbox_message_id_inited && read_inbox_max_message_id.is_valid())) {
      if (d->server_unread_count != dialog->unread_count_ ||
          d->last_read_inbox_message_id.get() < read_inbox_max_message_id.get()) {
        set_dialog_last_read_inbox_message_id(d, read_inbox_max_message_id, dialog->unread_count_,
                                              d->local_unread_count, true, "on_get_dialogs");
      }
    }

    if (!G()->parameters().use_message_db || is_new ||
        (!d->is_last_read_outbox_message_id_inited && read_outbox_max_message_id.is_valid())) {
      if (d->last_read_outbox_message_id.get() < read_outbox_max_message_id.get()) {
        set_dialog_last_read_outbox_message_id(d, read_outbox_max_message_id);
      }
    }

    if (!G()->parameters().use_message_db || is_new) {
      if (d->unread_mention_count != dialog->unread_mentions_count_) {
        d->unread_mention_count = dialog->unread_mentions_count_;
        d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] =
            d->unread_mention_count;
        send_update_chat_unread_mention_count(d);
      }
    }
  }
  if (from_pinned_dialog_list) {
    auto pinned_dialog_ids = remove_secret_chat_dialog_ids(get_pinned_dialogs());
    std::reverse(pinned_dialog_ids.begin(), pinned_dialog_ids.end());
    if (pinned_dialog_ids != added_dialog_ids) {
      LOG(INFO) << "Repair pinned dialogs order from " << format::as_array(pinned_dialog_ids) << " to "
                << format::as_array(added_dialog_ids);

      std::unordered_set<DialogId, DialogIdHash> old_pinned_dialog_ids(pinned_dialog_ids.begin(),
                                                                       pinned_dialog_ids.end());

      auto old_it = pinned_dialog_ids.begin();
      for (auto dialog_id : added_dialog_ids) {
        old_pinned_dialog_ids.erase(dialog_id);
        while (old_it < pinned_dialog_ids.end()) {
          if (*old_it == dialog_id) {
            break;
          }
          old_it++;
        }
        if (old_it < pinned_dialog_ids.end()) {
          // leave dialog where it is
          continue;
        }
        set_dialog_is_pinned(dialog_id, true);
      }
      for (auto dialog_id : old_pinned_dialog_ids) {
        set_dialog_is_pinned(dialog_id, false);
      }
    }
  }
  promise.set_value(Unit());
}

constexpr bool MessagesManager::is_debug_message_op_enabled() {
  return !LOG_IS_STRIPPED(ERROR) && false;
}

void MessagesManager::dump_debug_message_op(const Dialog *d, int priority) {
  if (!is_debug_message_op_enabled()) {
    return;
  }
  if (d == nullptr) {
    LOG(ERROR) << "Chat not found";
    return;
  }
  static int last_dumped_priority = -1;
  if (priority <= last_dumped_priority) {
    LOG(ERROR) << "Skip dump " << d->dialog_id;
    return;
  }
  last_dumped_priority = priority;

  for (auto &op : d->debug_message_op) {
    switch (op.type) {
      case Dialog::MessageOp::Add: {
        LOG(ERROR) << "MessageOpAdd at " << op.date << " " << op.message_id << " " << op.content_type << " "
                   << op.from_update << " " << op.have_previous << " " << op.have_next << " " << op.source;
        break;
      }
      case Dialog::MessageOp::SetPts: {
        LOG(ERROR) << "MessageOpSetPts at " << op.date << " " << op.content_type << " " << op.source;
        break;
      }
      case Dialog::MessageOp::Delete: {
        LOG(ERROR) << "MessageOpDelete at " << op.date << " " << op.message_id << " " << op.content_type << " "
                   << op.from_update << " " << op.have_previous << " " << op.have_next << " " << op.source;
        break;
      }
      case Dialog::MessageOp::DeleteAll: {
        LOG(ERROR) << "MessageOpDeleteAll at " << op.date << " " << op.from_update;
        break;
      }
      default:
        UNREACHABLE();
    }
  }
}

bool MessagesManager::is_message_unload_enabled() const {
  return G()->parameters().use_message_db || td_->auth_manager_->is_bot();
}

bool MessagesManager::can_unload_message(const Dialog *d, const Message *m) const {
  // don't want to unload messages from opened dialogs
  // don't want to unload messages to which there are replies in yet unsent messages
  // don't want to unload messages with pending web pages
  // can't unload from memory last dialog, last database messages, yet unsent messages and active live locations
  FullMessageId full_message_id{d->dialog_id, m->message_id};
  return !d->is_opened && m->message_id != d->last_message_id && m->message_id != d->last_database_message_id &&
         !m->message_id.is_yet_unsent() && active_live_location_full_message_ids_.count(full_message_id) == 0 &&
         replied_by_yet_unsent_messages_.count(full_message_id) == 0 &&
         waiting_for_web_page_messages_.count(full_message_id) == 0;
}

void MessagesManager::unload_message(Dialog *d, MessageId message_id) {
  bool need_update_dialog_pos = false;
  auto m = do_delete_message(d, message_id, false, true, &need_update_dialog_pos, "unload_message");
  CHECK(!need_update_dialog_pos);
}

unique_ptr<MessagesManager::Message> MessagesManager::delete_message(Dialog *d, MessageId message_id,
                                                                     bool is_permanently_deleted,
                                                                     bool *need_update_dialog_pos, const char *source) {
  return do_delete_message(d, message_id, is_permanently_deleted, false, need_update_dialog_pos, source);
}

// DO NOT FORGET TO ADD ALL CHANGES OF THIS FUNCTION AS WELL TO do_delete_all_dialog_messages
unique_ptr<MessagesManager::Message> MessagesManager::do_delete_message(Dialog *d, MessageId message_id,
                                                                        bool is_permanently_deleted,
                                                                        bool only_from_memory,
                                                                        bool *need_update_dialog_pos,
                                                                        const char *source) {
  if (!message_id.is_valid()) {
    LOG(ERROR) << "Trying to delete " << message_id << " in " << d->dialog_id << " from " << source;
    return nullptr;
  }

  FullMessageId full_message_id(d->dialog_id, message_id);
  unique_ptr<Message> *v = find_message(&d->messages, message_id);
  if (*v == nullptr) {
    LOG(INFO) << message_id << " is not found in " << d->dialog_id << " to be deleted from " << source;
    if (only_from_memory) {
      return nullptr;
    }

    if (get_message_force(d, message_id) == nullptr) {
      // currently there may be a race between add_message_to_database and get_message_force,
      // so delete a message from database just in case
      delete_message_from_database(d, message_id, nullptr, is_permanently_deleted);

      if (is_permanently_deleted && d->last_clear_history_message_id == message_id) {
        set_dialog_last_clear_history_date(d, 0, MessageId(), "do_delete_message");
        on_dialog_updated(d->dialog_id, "forget last_clear_history_date from do_delete_message");
        *need_update_dialog_pos = true;
      }

      /*
      can't do this because the message may be never received in the dialog, unread count will became negative
      // if last_read_inbox_message_id is not known, we can't be sure whether unread_count should be decreased or not
      if (message_id.is_valid() && !message_id.is_yet_unsent() && d->is_last_read_inbox_message_id_inited &&
          message_id.get() > d->last_read_inbox_message_id.get() && !td_->auth_manager_->is_bot()) {
        int32 server_unread_count = d->server_unread_count;
        int32 local_unread_count = d->local_unread_count;
        int32 &unread_count = message_id.is_server() ? server_unread_count : local_unread_count;
        if (unread_count == 0) {
          LOG(ERROR) << "Unread count became negative in " << d->dialog_id << " after deletion of " << message_id
                     << ". Last read is " << d->last_read_inbox_message_id;
          dump_debug_message_op(d, 3);
        } else {
          unread_count--;
          set_dialog_last_read_inbox_message_id(d, MessageId::min(), server_unread_count, local_unread_count, false,
                                                source);
        }
      }
      */
      return nullptr;
    }
    v = find_message(&d->messages, message_id);
    CHECK(*v != nullptr);
  }

  const Message *m = v->get();
  if (only_from_memory && !can_unload_message(d, m)) {
    return nullptr;
  }

  if (is_debug_message_op_enabled()) {
    d->debug_message_op.emplace_back(Dialog::MessageOp::Delete, m->message_id, m->content->get_id(), false,
                                     m->have_previous, m->have_next, source);
  }

  LOG(INFO) << "Deleting " << full_message_id << " with have_previous = " << m->have_previous
            << " and have_next = " << m->have_next << " from " << source;

  bool need_get_history = false;
  if (!only_from_memory) {
    delete_message_from_database(d, message_id, m, is_permanently_deleted);

    delete_active_live_location(d->dialog_id, m);

    if (message_id == d->last_message_id) {
      MessagesConstIterator it(d, message_id);
      CHECK(*it == m);
      if ((*it)->have_previous) {
        --it;
        if (*it != nullptr) {
          set_dialog_last_message_id(d, (*it)->message_id, "do_delete_message");
        } else {
          LOG(ERROR) << "have_previous is true, but there is no previous for " << full_message_id << " from " << source;
          dump_debug_message_op(d);
          set_dialog_last_message_id(d, MessageId(), "do_delete_message");
        }
      } else {
        need_get_history = true;
        set_dialog_last_message_id(d, MessageId(), "do_delete_message");
        d->delete_last_message_date = m->date;
        d->deleted_last_message_id = message_id;
        d->is_last_message_deleted_locally = Slice(source) == Slice(DELETE_MESSAGE_USER_REQUEST_SOURCE);
        on_dialog_updated(d->dialog_id, "do delete last message");
      }
      *need_update_dialog_pos = true;
    }

    if (message_id == d->last_database_message_id) {
      MessagesConstIterator it(d, message_id);
      CHECK(*it == m);
      while ((*it)->have_previous) {
        --it;
        if (*it == nullptr || !(*it)->message_id.is_yet_unsent()) {
          break;
        }
      }

      if (*it != nullptr) {
        if (!(*it)->message_id.is_yet_unsent() && (*it)->message_id != d->last_database_message_id) {
          set_dialog_last_database_message_id(d, (*it)->message_id, "do_delete_message");
        } else {
          need_get_history = true;
        }
      } else {
        LOG(ERROR) << "have_previous is true, but there is no previous";
        dump_debug_message_op(d);
      }
      on_dialog_updated(d->dialog_id, "do delete last database message");
    }
    if (d->last_database_message_id.is_valid()) {
      CHECK(d->first_database_message_id.is_valid());
    } else {
      set_dialog_first_database_message_id(d, MessageId(), "do_delete_message");
    }
  }

  if (m->have_previous && (only_from_memory || !m->have_next)) {
    MessagesIterator it(d, message_id);
    CHECK(*it == m);
    --it;
    Message *prev_m = *it;
    if (prev_m != nullptr) {
      prev_m->have_next = false;
    } else {
      LOG(ERROR) << "have_previous is true, but there is no previous for " << full_message_id << " from " << source;
      dump_debug_message_op(d);
    }
  }
  if ((*v)->have_next && (only_from_memory || !(*v)->have_previous)) {
    MessagesIterator it(d, message_id);
    CHECK(*it == m);
    ++it;
    Message *next_m = *it;
    if (next_m != nullptr) {
      next_m->have_previous = false;
    } else {
      LOG(ERROR) << "have_next is true, but there is no next for " << full_message_id << " from " << source;
      dump_debug_message_op(d);
    }
  }

  unique_ptr<Message> result = std::move(*v);
  unique_ptr<Message> left = std::move(result->left);
  unique_ptr<Message> right = std::move(result->right);

  while (left != nullptr || right != nullptr) {
    if (left == nullptr || (right != nullptr && right->random_y > left->random_y)) {
      *v = std::move(right);
      v = &((*v)->left);
      right = std::move(*v);
    } else {
      *v = std::move(left);
      v = &((*v)->right);
      left = std::move(*v);
    }
  }
  CHECK(*v == nullptr);

  if (!only_from_memory) {
    if (message_id.is_yet_unsent()) {
      cancel_send_message_query(d->dialog_id, result);
    }

    if (need_get_history && !td_->auth_manager_->is_bot() && have_input_peer(d->dialog_id, AccessRights::Read)) {
      get_history_from_the_end(d->dialog_id, true, false, Auto());
    }

    if (d->reply_markup_message_id == message_id) {
      set_dialog_reply_markup(d, MessageId());
    }
    // if last_read_inbox_message_id is not known, we can't be sure whether unread_count should be decreased or not
    if (!result->is_outgoing && message_id.get() > d->last_read_inbox_message_id.get() &&
        d->dialog_id != DialogId(td_->contacts_manager_->get_my_id("do_delete_message")) &&
        d->is_last_read_inbox_message_id_inited && !td_->auth_manager_->is_bot()) {
      int32 server_unread_count = d->server_unread_count;
      int32 local_unread_count = d->local_unread_count;
      int32 &unread_count = message_id.is_server() ? server_unread_count : local_unread_count;
      if (unread_count == 0) {
        LOG(ERROR) << "Unread count became negative in " << d->dialog_id << " after deletion of " << message_id
                   << ". Last read is " << d->last_read_inbox_message_id;
        dump_debug_message_op(d, 3);
      } else {
        unread_count--;
        set_dialog_last_read_inbox_message_id(d, MessageId::min(), server_unread_count, local_unread_count, false,
                                              source);
      }
    }
    if (result->contains_unread_mention) {
      if (d->unread_mention_count == 0) {
        LOG_IF(ERROR,
               d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] != -1)
            << "Unread mention count became negative in " << d->dialog_id << " after deletion of " << message_id;
      } else {
        d->unread_mention_count--;
        d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] =
            d->unread_mention_count;
        send_update_chat_unread_mention_count(d);
      }
    }

    update_message_count_by_index(d, -1, result.get());
  }

  switch (d->dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      message_id_to_dialog_id_.erase(message_id);
      break;
    case DialogType::Channel:
      // nothing to do
      break;
    case DialogType::SecretChat:
      LOG(INFO) << "Delete correspondence random_id " << result->random_id << " to " << message_id << " in "
                << d->dialog_id;
      d->random_id_to_message_id.erase(result->random_id);
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }
  ttl_unregister_message(d->dialog_id, result.get(), Time::now());

  return result;
}

void MessagesManager::do_delete_all_dialog_messages(Dialog *d, unique_ptr<Message> &m,
                                                    vector<int64> &deleted_message_ids) {
  if (m == nullptr) {
    return;
  }
  MessageId message_id = m->message_id;

  if (is_debug_message_op_enabled()) {
    d->debug_message_op.emplace_back(Dialog::MessageOp::Delete, m->message_id, m->content->get_id(), false,
                                     m->have_previous, m->have_next, "delete all messages");
  }

  LOG(INFO) << "Delete " << message_id;
  deleted_message_ids.push_back(message_id.get());

  do_delete_all_dialog_messages(d, m->right, deleted_message_ids);
  do_delete_all_dialog_messages(d, m->left, deleted_message_ids);

  delete_active_live_location(d->dialog_id, m.get());

  if (message_id.is_yet_unsent()) {
    cancel_send_message_query(d->dialog_id, m);
  }

  switch (d->dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      message_id_to_dialog_id_.erase(message_id);
      break;
    case DialogType::Channel:
      // nothing to do
      break;
    case DialogType::SecretChat:
      LOG(INFO) << "Delete correspondence random_id " << m->random_id << " to " << message_id << " in " << d->dialog_id;
      d->random_id_to_message_id.erase(m->random_id);
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }
  ttl_unregister_message(d->dialog_id, m.get(), Time::now());

  m = nullptr;
}

bool MessagesManager::have_dialog(DialogId dialog_id) const {
  return dialogs_.count(dialog_id) > 0;
}

void MessagesManager::load_dialogs(vector<DialogId> dialog_ids, Promise<Unit> &&promise) {
  LOG(INFO) << "Load dialogs " << format::as_array(dialog_ids);

  Dependencies dependencies;
  for (auto dialog_id : dialog_ids) {
    if (dialog_id.is_valid() && !have_dialog(dialog_id)) {
      add_dialog_dependencies(dependencies, dialog_id);
    }
  }
  resolve_dependencies_force(dependencies);

  for (auto dialog_id : dialog_ids) {
    if (dialog_id.is_valid()) {
      force_create_dialog(dialog_id, "load_dialogs");
    }
  }

  promise.set_value(Unit());
}

bool MessagesManager::load_dialog(DialogId dialog_id, int left_tries, Promise<Unit> &&promise) {
  if (!dialog_id.is_valid()) {
    promise.set_error(Status::Error(6, "Invalid chat identifier"));
    return false;
  }

  if (!have_dialog_force(dialog_id)) {  // TODO remove _force
    if (G()->parameters().use_message_db) {
      //      TODO load dialog from database, DialogLoader
      //      send_closure_later(actor_id(this), &MessagesManager::load_dialog_from_database, dialog_id,
      //      std::move(promise));
      //      return false;
    }
    if (td_->auth_manager_->is_bot()) {
      switch (dialog_id.get_type()) {
        case DialogType::User: {
          auto user_id = dialog_id.get_user_id();
          auto have_user = td_->contacts_manager_->get_user(user_id, left_tries, std::move(promise));
          if (!have_user) {
            return false;
          }
          break;
        }
        case DialogType::Chat: {
          auto have_chat = td_->contacts_manager_->get_chat(dialog_id.get_chat_id(), left_tries, std::move(promise));
          if (!have_chat) {
            return false;
          }
          break;
        }
        case DialogType::Channel: {
          auto have_channel =
              td_->contacts_manager_->get_channel(dialog_id.get_channel_id(), left_tries, std::move(promise));
          if (!have_channel) {
            return false;
          }
          break;
        }
        case DialogType::SecretChat:
          promise.set_error(Status::Error(6, "Chat not found"));
          return false;
        case DialogType::None:
        default:
          UNREACHABLE();
      }
      if (!have_input_peer(dialog_id, AccessRights::Read)) {
        return false;
      }

      add_dialog(dialog_id);
      return true;
    }

    promise.set_error(Status::Error(6, "Chat not found"));
    return false;
  }

  promise.set_value(Unit());
  return true;
}

vector<DialogId> MessagesManager::get_dialogs(DialogDate offset, int32 limit, bool force, Promise<Unit> &&promise) {
  LOG(INFO) << "Get chats with offset " << offset << " and limit " << limit << ". Know about order of "
            << ordered_dialogs_.size() << " chat(s). last_dialog_date_ = " << last_dialog_date_
            << ", last_server_dialog_date_ = " << last_server_dialog_date_
            << ", last_loaded_database_dialog_date_ = " << last_loaded_database_dialog_date_;

  vector<DialogId> result;
  if (limit <= 0) {
    promise.set_error(Status::Error(3, "Parameter limit in getChats must be positive"));
    return result;
  }

  if (limit > MAX_GET_DIALOGS) {
    limit = MAX_GET_DIALOGS;
  }

  auto it = ordered_dialogs_.upper_bound(offset);
  auto end = ordered_dialogs_.end();
  while (it != end && limit-- > 0) {
    result.push_back(it->get_dialog_id());
    ++it;
  }

  if (limit <= 0 || last_dialog_date_ == MAX_DIALOG_DATE || force) {
    promise.set_value(Unit());
    return result;
  }

  load_dialog_list(std::move(promise));
  return result;
}

void MessagesManager::load_dialog_list(Promise<Unit> &&promise) {
  auto &multipromise = load_dialog_list_multipromise_;
  multipromise.add_promise(std::move(promise));
  if (multipromise.promise_count() != 1) {
    // queries have already been sent, just wait for the result
    return;
  }

  bool is_query_sent = false;
  if (G()->parameters().use_message_db && last_loaded_database_dialog_date_ < last_database_server_dialog_date_) {
    load_dialog_list_from_database(MAX_GET_DIALOGS, multipromise.get_promise());
    is_query_sent = true;
  } else {
    LOG(INFO) << "Get dialogs from " << last_server_dialog_date_;
    auto sequence_id = get_sequence_dispatcher_id(DialogId(), -1);
    send_closure(td_->create_net_actor<GetPinnedDialogsQuery>(multipromise.get_promise()), &GetPinnedDialogsQuery::send,
                 sequence_id);
    if (last_dialog_date_ == last_server_dialog_date_) {
      send_closure(td_->create_net_actor<GetDialogListQuery>(multipromise.get_promise()), &GetDialogListQuery::send,
                   last_server_dialog_date_.get_date(),
                   last_server_dialog_date_.get_message_id().get_next_server_message_id().get_server_message_id(),
                   last_server_dialog_date_.get_dialog_id(), int32{MAX_GET_DIALOGS}, sequence_id);
      is_query_sent = true;
    }
  }
  CHECK(is_query_sent);
}

void MessagesManager::load_dialog_list_from_database(int32 limit, Promise<Unit> &&promise) {
  LOG(INFO) << "Load dialogs from " << last_loaded_database_dialog_date_
            << ", last database server dialog date = " << last_database_server_dialog_date_;

  G()->td_db()->get_dialog_db_async()->get_dialogs(
      last_loaded_database_dialog_date_.get_order(), last_loaded_database_dialog_date_.get_dialog_id(), limit,
      PromiseCreator::lambda([actor_id = actor_id(this),
                              promise = std::move(promise)](vector<BufferSlice> result) mutable {
        send_closure(actor_id, &MessagesManager::on_get_dialogs_from_database, std::move(result), std::move(promise));
      }));
}

void MessagesManager::on_get_dialogs_from_database(vector<BufferSlice> &&dialogs, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive " << dialogs.size() << " dialogs in result of GetDialogsFromDatabase";
  DialogDate max_dialog_date = MIN_DIALOG_DATE;
  for (auto &dialog : dialogs) {
    Dialog *d = on_load_dialog_from_database(std::move(dialog));
    CHECK(d != nullptr);

    DialogDate dialog_date(d->order, d->dialog_id);
    if (max_dialog_date < dialog_date) {
      max_dialog_date = dialog_date;
    }
    LOG(INFO) << "Chat " << dialog_date << " is loaded from database";
  }

  if (dialogs.empty()) {
    // if there is no more dialogs in the database
    last_loaded_database_dialog_date_ = MAX_DIALOG_DATE;
    LOG(INFO) << "Set last loaded database dialog date to " << last_loaded_database_dialog_date_;
    last_server_dialog_date_ = max(last_server_dialog_date_, last_database_server_dialog_date_);
    LOG(INFO) << "Set last server dialog date to " << last_server_dialog_date_;
    update_last_dialog_date();
  }
  if (last_loaded_database_dialog_date_ < max_dialog_date) {
    last_loaded_database_dialog_date_ = min(max_dialog_date, last_database_server_dialog_date_);
    LOG(INFO) << "Set last loaded database dialog date to " << last_loaded_database_dialog_date_;
    last_server_dialog_date_ = max(last_server_dialog_date_, last_loaded_database_dialog_date_);
    LOG(INFO) << "Set last server dialog date to " << last_server_dialog_date_;
    update_last_dialog_date();
  } else if (!dialogs.empty()) {
    LOG(ERROR) << "Last loaded database dialog date didn't increased";
  }

  if (!preload_dialog_list_timeout_.has_timeout()) {
    LOG(INFO) << "Schedule chat list preload";
    preload_dialog_list_timeout_.set_callback(std::move(MessagesManager::preload_dialog_list));
    preload_dialog_list_timeout_.set_callback_data(static_cast<void *>(this));
  }
  preload_dialog_list_timeout_.set_timeout_in(0.2);

  promise.set_value(Unit());
}

void MessagesManager::preload_dialog_list(void *messages_manager_void) {
  if (G()->close_flag()) {
    return;
  }

  CHECK(messages_manager_void != nullptr);
  auto messages_manager = static_cast<MessagesManager *>(messages_manager_void);

  CHECK(G()->parameters().use_message_db);
  if (messages_manager->load_dialog_list_multipromise_.promise_count() != 0) {
    // do nothing if there is pending load dialog list request
    return;
  }

  if (messages_manager->ordered_dialogs_.size() > MAX_PRELOADED_DIALOGS) {
    // do nothing if there are more than MAX_PRELOADED_DIALOGS dialogs already loaded
    messages_manager->recalc_unread_message_count();
    return;
  }

  if (messages_manager->last_loaded_database_dialog_date_ < messages_manager->last_database_server_dialog_date_) {
    // if there are some dialogs in database, preload some of them
    messages_manager->load_dialog_list_from_database(20, Auto());
  } else if (messages_manager->last_dialog_date_ != MAX_DIALOG_DATE) {
    messages_manager->load_dialog_list(PromiseCreator::lambda([messages_manager](Result<Unit> result) {
      if (result.is_ok()) {
        messages_manager->recalc_unread_message_count();
      }
    }));
  } else {
    messages_manager->recalc_unread_message_count();
  }
}

vector<DialogId> MessagesManager::get_pinned_dialogs() const {
  vector<DialogId> result;

  auto it = ordered_dialogs_.begin();
  auto end = ordered_dialogs_.end();
  while (it != end && it->get_date() >= MIN_PINNED_DIALOG_DATE) {
    result.push_back(it->get_dialog_id());
    ++it;
  }

  return result;
}

vector<DialogId> MessagesManager::search_public_dialogs(const string &query, Promise<Unit> &&promise) {
  LOG(INFO) << "Search public chats with query = \"" << query << '"';

  if (utf8_length(query) < MIN_SEARCH_PUBLIC_DIALOG_PREFIX_LEN) {
    string username = clean_username(query);
    if (username[0] == '@') {
      username = username.substr(1);
    }

    for (auto &short_username : get_valid_short_usernames()) {
      if (2 * username.size() > short_username.size() && begins_with(short_username, username)) {
        username = short_username.str();
        auto it = resolved_usernames_.find(username);
        if (it == resolved_usernames_.end()) {
          td_->create_handler<ResolveUsernameQuery>(std::move(promise))->send(username);
          return {};
        }

        if (it->second.expires_at < Time::now()) {
          td_->create_handler<ResolveUsernameQuery>(Promise<>())->send(username);
        }

        auto dialog_id = it->second.dialog_id;
        force_create_dialog(dialog_id, "public dialogs search");
        promise.set_value(Unit());
        return {dialog_id};
      }
    }
    promise.set_value(Unit());
    return {};
  }

  auto it = found_public_dialogs_.find(query);
  if (it != found_public_dialogs_.end()) {
    promise.set_value(Unit());
    return it->second;
  }

  send_search_public_dialogs_query(query, std::move(promise));
  return vector<DialogId>();
}

void MessagesManager::send_search_public_dialogs_query(const string &query, Promise<Unit> &&promise) {
  auto &promises = search_public_dialogs_queries_[query];
  promises.push_back(std::move(promise));
  if (promises.size() != 1) {
    // query has already been sent, just wait for the result
    return;
  }

  td_->create_handler<SearchPublicDialogsQuery>()->send(query);
}

std::pair<size_t, vector<DialogId>> MessagesManager::search_dialogs(const string &query, int32 limit,
                                                                    Promise<Unit> &&promise) {
  LOG(INFO) << "Search chats with query \"" << query << "\" and limit " << limit;

  if (limit < 0) {
    promise.set_error(Status::Error(400, "Limit must be non-negative"));
    return {};
  }
  if (query.empty()) {
    if (!load_recently_found_dialogs(promise)) {
      return {};
    }

    promise.set_value(Unit());
    size_t result_size = min(static_cast<size_t>(limit), recently_found_dialog_ids_.size());
    return {recently_found_dialog_ids_.size(),
            vector<DialogId>(recently_found_dialog_ids_.begin(), recently_found_dialog_ids_.begin() + result_size)};
  }

  auto result = dialogs_hints_.search(query, limit);
  vector<DialogId> dialog_ids;
  dialog_ids.reserve(result.second.size());
  for (auto key : result.second) {
    dialog_ids.push_back(DialogId(-key));
  }

  promise.set_value(Unit());
  return {result.first, std::move(dialog_ids)};
}

vector<DialogId> MessagesManager::sort_dialogs_by_order(const vector<DialogId> &dialog_ids, int32 limit) const {
  auto dialog_dates = transform(dialog_ids, [this](auto dialog_id) {
    const Dialog *d = this->get_dialog(dialog_id);
    CHECK(d != nullptr);
    return DialogDate(d->order, dialog_id);
  });
  if (static_cast<size_t>(limit) >= dialog_dates.size()) {
    std::sort(dialog_dates.begin(), dialog_dates.end());
  } else {
    std::partial_sort(dialog_dates.begin(), dialog_dates.begin() + limit, dialog_dates.end());
    dialog_dates.resize(limit, MAX_DIALOG_DATE);
  }
  return transform(dialog_dates, [](auto dialog_date) { return dialog_date.get_dialog_id(); });
}

vector<DialogId> MessagesManager::search_dialogs_on_server(const string &query, int32 limit, Promise<Unit> &&promise) {
  LOG(INFO) << "Search chats on server with query \"" << query << "\" and limit " << limit;

  if (limit < 0) {
    promise.set_error(Status::Error(400, "Limit must be non-negative"));
    return {};
  }
  if (limit > MAX_GET_DIALOGS) {
    limit = MAX_GET_DIALOGS;
  }

  if (query.empty()) {
    promise.set_value(Unit());
    return {};
  }

  auto it = found_on_server_dialogs_.find(query);
  if (it != found_on_server_dialogs_.end()) {
    promise.set_value(Unit());
    return sort_dialogs_by_order(it->second, limit);
  }

  send_search_public_dialogs_query(query, std::move(promise));
  return vector<DialogId>();
}

vector<DialogId> MessagesManager::get_common_dialogs(UserId user_id, DialogId offset_dialog_id, int32 limit, bool force,
                                                     Promise<Unit> &&promise) {
  if (!td_->contacts_manager_->have_input_user(user_id)) {
    promise.set_error(Status::Error(6, "Have no access to the user"));
    return vector<DialogId>();
  }

  if (user_id == td_->contacts_manager_->get_my_id("get_common_dialogs")) {
    promise.set_error(Status::Error(6, "Can't get common chats with self"));
    return vector<DialogId>();
  }
  if (limit <= 0) {
    promise.set_error(Status::Error(3, "Parameter limit must be positive"));
    return vector<DialogId>();
  }
  if (limit > MAX_GET_DIALOGS) {
    limit = MAX_GET_DIALOGS;
  }

  int32 offset_chat_id = 0;
  switch (offset_dialog_id.get_type()) {
    case DialogType::Chat:
      offset_chat_id = offset_dialog_id.get_chat_id().get();
      break;
    case DialogType::Channel:
      offset_chat_id = offset_dialog_id.get_channel_id().get();
      break;
    case DialogType::None:
      if (offset_dialog_id == DialogId()) {
        break;
      }
    // fallthrough
    case DialogType::User:
    case DialogType::SecretChat:
      promise.set_error(Status::Error(6, "Wrong offset_chat_id"));
      return vector<DialogId>();
    default:
      UNREACHABLE();
      break;
  }

  auto it = found_common_dialogs_.find(user_id);
  if (it != found_common_dialogs_.end() && !it->second.empty()) {
    vector<DialogId> &common_dialog_ids = it->second;
    auto offset_it = common_dialog_ids.begin();
    if (offset_dialog_id != DialogId()) {
      offset_it = std::find(common_dialog_ids.begin(), common_dialog_ids.end(), offset_dialog_id);
      if (offset_it == common_dialog_ids.end()) {
        promise.set_error(Status::Error(6, "Wrong offset_chat_id"));
        return vector<DialogId>();
      }
      ++offset_it;
    }
    vector<DialogId> result;
    while (result.size() < static_cast<size_t>(limit)) {
      if (offset_it == common_dialog_ids.end()) {
        break;
      }
      auto dialog_id = *offset_it++;
      if (dialog_id == DialogId()) {  // end of the list
        promise.set_value(Unit());
        return result;
      }
      result.push_back(dialog_id);
    }
    if (result.size() == static_cast<size_t>(limit) || force) {
      promise.set_value(Unit());
      return result;
    }
  }

  td_->create_handler<GetCommonDialogsQuery>(std::move(promise))->send(user_id, offset_chat_id, limit);
  return vector<DialogId>();
}

void MessagesManager::on_get_common_dialogs(UserId user_id, vector<tl_object_ptr<telegram_api::Chat>> &&chats,
                                            int32 total_count) {
  auto &result = found_common_dialogs_[user_id];
  if (result.size() > 0 && result.back() == DialogId()) {
    return;
  }
  for (auto &chat : chats) {
    DialogId dialog_id;
    switch (chat->get_id()) {
      case telegram_api::chatEmpty::ID: {
        auto c = static_cast<const telegram_api::chatEmpty *>(chat.get());
        ChatId chat_id(c->id_);
        if (!chat_id.is_valid()) {
          LOG(ERROR) << "Receive invalid " << chat_id;
          continue;
        }
        dialog_id = DialogId(chat_id);
        break;
      }
      case telegram_api::chat::ID: {
        auto c = static_cast<const telegram_api::chat *>(chat.get());
        ChatId chat_id(c->id_);
        if (!chat_id.is_valid()) {
          LOG(ERROR) << "Receive invalid " << chat_id;
          continue;
        }
        dialog_id = DialogId(chat_id);
        break;
      }
      case telegram_api::chatForbidden::ID: {
        auto c = static_cast<const telegram_api::chatForbidden *>(chat.get());
        ChatId chat_id(c->id_);
        if (!chat_id.is_valid()) {
          LOG(ERROR) << "Receive invalid " << chat_id;
          continue;
        }
        dialog_id = DialogId(chat_id);
        break;
      }
      case telegram_api::channel::ID: {
        auto c = static_cast<const telegram_api::channel *>(chat.get());
        ChannelId channel_id(c->id_);
        if (!channel_id.is_valid()) {
          LOG(ERROR) << "Receive invalid " << channel_id;
          continue;
        }
        dialog_id = DialogId(channel_id);
        break;
      }
      case telegram_api::channelForbidden::ID: {
        auto c = static_cast<const telegram_api::channelForbidden *>(chat.get());
        ChannelId channel_id(c->id_);
        if (!channel_id.is_valid()) {
          LOG(ERROR) << "Receive invalid " << channel_id;
          continue;
        }
        dialog_id = DialogId(channel_id);
        break;
      }
      default:
        UNREACHABLE();
    }
    CHECK(dialog_id.is_valid());
    td_->contacts_manager_->on_get_chat(std::move(chat));

    if (std::find(result.begin(), result.end(), dialog_id) == result.end()) {
      force_create_dialog(dialog_id, "get common dialogs");
      result.push_back(dialog_id);
    }
  }
  if (result.size() >= static_cast<size_t>(total_count)) {
    result.push_back(DialogId());
  }
}

bool MessagesManager::have_message(FullMessageId full_message_id) {
  return get_message_force(full_message_id) != nullptr;
}

MessagesManager::Message *MessagesManager::get_message(FullMessageId full_message_id) {
  Dialog *d = get_dialog(full_message_id.get_dialog_id());
  if (d == nullptr) {
    return nullptr;
  }

  return get_message(d, full_message_id.get_message_id());
}

const MessagesManager::Message *MessagesManager::get_message(FullMessageId full_message_id) const {
  const Dialog *d = get_dialog(full_message_id.get_dialog_id());
  if (d == nullptr) {
    return nullptr;
  }

  return get_message(d, full_message_id.get_message_id());
}

MessagesManager::Message *MessagesManager::get_message_force(FullMessageId full_message_id) {
  Dialog *d = get_dialog_force(full_message_id.get_dialog_id());
  if (d == nullptr) {
    return nullptr;
  }

  return get_message_force(d, full_message_id.get_message_id());
}

MessageId MessagesManager::get_replied_message_id(const Message *m) {
  switch (m->content->get_id()) {
    case MessagePinMessage::ID:
      CHECK(!m->reply_to_message_id.is_valid());
      return static_cast<const MessagePinMessage *>(m->content.get())->message_id;
    case MessageGameScore::ID:
      CHECK(!m->reply_to_message_id.is_valid());
      return static_cast<const MessageGameScore *>(m->content.get())->game_message_id;
    case MessagePaymentSuccessful::ID:
      CHECK(!m->reply_to_message_id.is_valid());
      return static_cast<const MessagePaymentSuccessful *>(m->content.get())->invoice_message_id;
    default:
      return m->reply_to_message_id;
  }
}

void MessagesManager::get_message_force_from_server(Dialog *d, MessageId message_id, Promise<Unit> &&promise,
                                                    tl_object_ptr<telegram_api::InputMessage> input_message) {
  auto m = get_message_force(d, message_id);
  if (m == nullptr && message_id.is_valid() && message_id.is_server()) {
    auto dialog_type = d->dialog_id.get_type();
    if (d->last_new_message_id != MessageId() && message_id.get() > d->last_new_message_id.get()) {
      // message will not be added to the dialog anyway
      if (dialog_type == DialogType::Channel) {
        // so we try to force channel difference first
        CHECK(input_message == nullptr);  // replied message can't be older than already added original message
        postponed_get_message_requests_[d->dialog_id].emplace_back(message_id, std::move(promise));
        get_channel_difference(d->dialog_id, d->pts, true, "get_message");
      } else {
        promise.set_value(Unit());
      }
      return;
    }

    if (d->deleted_message_ids.count(message_id) == 0 && dialog_type != DialogType::SecretChat) {
      return get_messages_from_server({FullMessageId(d->dialog_id, message_id)}, std::move(promise),
                                      std::move(input_message));
    }
  }

  promise.set_value(Unit());
}

void MessagesManager::get_message(FullMessageId full_message_id, Promise<Unit> &&promise) {
  Dialog *d = get_dialog_force(full_message_id.get_dialog_id());
  if (d == nullptr) {
    return promise.set_error(Status::Error(6, "Chat not found"));
  }

  get_message_force_from_server(d, full_message_id.get_message_id(), std::move(promise));
}

MessageId MessagesManager::get_replied_message(DialogId dialog_id, MessageId message_id, bool force,
                                               Promise<Unit> &&promise) {
  LOG(INFO) << "Get replied message to " << message_id << " in " << dialog_id;
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(6, "Chat not found"));
    return MessageId();
  }

  auto m = get_message_force(d, message_id);
  if (m == nullptr) {
    if (force) {
      promise.set_value(Unit());
    } else {
      get_message_force_from_server(d, message_id, std::move(promise));
    }
    return MessageId();
  }

  tl_object_ptr<telegram_api::InputMessage> input_message;
  if (message_id.is_server()) {
    input_message = make_tl_object<telegram_api::inputMessageReplyTo>(message_id.get_server_message_id().get());
  }
  auto replied_message_id = get_replied_message_id(m);
  get_message_force_from_server(d, replied_message_id, std::move(promise), std::move(input_message));

  return replied_message_id;
}

void MessagesManager::get_dialog_pinned_message(DialogId dialog_id, Promise<MessageId> &&promise) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(6, "Chat not found"));
  }

  if (dialog_id.get_type() != DialogType::Channel) {
    return promise.set_value(MessageId());
  }

  auto channel_id = dialog_id.get_channel_id();
  auto message_id = td_->contacts_manager_->get_channel_pinned_message_id(channel_id);
  if (get_message_force(d, message_id) == nullptr) {
    return td_->create_handler<GetChannelPinnedMessageQuery>(std::move(promise))->send(channel_id);
  }

  promise.set_value(std::move(message_id));
}

bool MessagesManager::get_messages(DialogId dialog_id, const vector<MessageId> &message_ids, Promise<Unit> &&promise) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(6, "Chat not found"));
    return false;
  }

  bool is_secret = dialog_id.get_type() == DialogType::SecretChat;
  vector<FullMessageId> missed_message_ids;
  for (auto message_id : message_ids) {
    if (!message_id.is_valid()) {
      promise.set_error(Status::Error(6, "Invalid message identifier"));
      return false;
    }

    auto message = get_message_force(d, message_id);
    if (message == nullptr && message_id.is_server() && !is_secret) {
      missed_message_ids.emplace_back(dialog_id, message_id);
      continue;
    }
  }

  if (!missed_message_ids.empty()) {
    get_messages_from_server(std::move(missed_message_ids), std::move(promise));
    return false;
  }

  promise.set_value(Unit());
  return true;
}

void MessagesManager::get_messages_from_server(vector<FullMessageId> &&message_ids, Promise<Unit> &&promise,
                                               tl_object_ptr<telegram_api::InputMessage> input_message) {
  if (message_ids.empty()) {
    LOG(ERROR) << "Empty message_ids";
    return;
  }

  if (input_message != nullptr) {
    CHECK(message_ids.size() == 1);
  }

  vector<tl_object_ptr<telegram_api::InputMessage>> ordinary_message_ids;
  std::unordered_map<ChannelId, vector<tl_object_ptr<telegram_api::InputMessage>>, ChannelIdHash> channel_message_ids;
  for (auto &full_message_id : message_ids) {
    auto dialog_id = full_message_id.get_dialog_id();
    auto message_id = full_message_id.get_message_id();
    if (!message_id.is_valid() || !message_id.is_server()) {
      continue;
    }

    switch (dialog_id.get_type()) {
      case DialogType::User:
      case DialogType::Chat:
        ordinary_message_ids.push_back(input_message == nullptr ? get_input_message(message_id)
                                                                : std::move(input_message));
        break;
      case DialogType::Channel:
        channel_message_ids[dialog_id.get_channel_id()].push_back(
            input_message == nullptr ? get_input_message(message_id) : std::move(input_message));
        break;
      case DialogType::SecretChat:
        LOG(ERROR) << "Can't get secret chat message from server";
        break;
      case DialogType::None:
      default:
        UNREACHABLE();
        break;
    }
  }

  // TODO MultiPromise
  size_t query_count = !ordinary_message_ids.empty() + channel_message_ids.size();
  LOG_IF(ERROR, query_count > 1 && promise) << "Promise will be called after first query returns";

  if (!ordinary_message_ids.empty()) {
    td_->create_handler<GetMessagesQuery>(std::move(promise))->send(std::move(ordinary_message_ids));
  }

  for (auto &it : channel_message_ids) {
    auto input_channel = td_->contacts_manager_->get_input_channel(it.first);
    if (input_channel == nullptr) {
      LOG(ERROR) << "Can't find info about " << it.first << " to get a message from it";
      promise.set_error(Status::Error(6, "Can't access the chat"));
      continue;
    }
    td_->create_handler<GetChannelMessagesQuery>(std::move(promise))
        ->send(it.first, std::move(input_channel), std::move(it.second));
  }
}

bool MessagesManager::is_message_edited_recently(FullMessageId full_message_id, int32 seconds) {
  if (seconds < 0) {
    return false;
  }

  auto m = get_message_force(full_message_id);
  if (m == nullptr) {
    return true;
  }

  return m->edit_date >= G()->unix_time() - seconds;
}

std::pair<string, string> MessagesManager::get_public_message_link(FullMessageId full_message_id, bool for_group,
                                                                   Promise<Unit> &&promise) {
  auto dialog_id = full_message_id.get_dialog_id();
  auto d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(6, "Chat not found"));
    return {};
  }
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    promise.set_error(Status::Error(6, "Can't access the chat"));
    return {};
  }
  if (dialog_id.get_type() != DialogType::Channel ||
      td_->contacts_manager_->get_channel_username(dialog_id.get_channel_id()).empty()) {
    promise.set_error(Status::Error(
        6, "Public message links are available only for messages in public supergroups and channel chats"));
    return {};
  }

  auto message_id = full_message_id.get_message_id();
  auto message = get_message_force(d, message_id);
  if (message == nullptr) {
    promise.set_error(Status::Error(6, "Message not found"));
    return {};
  }
  if (!message_id.is_server()) {
    promise.set_error(Status::Error(6, "Message is local"));
    return {};
  }

  auto it = public_message_links_[for_group].find(full_message_id);
  if (it == public_message_links_[for_group].end()) {
    td_->create_handler<ExportChannelMessageLinkQuery>(std::move(promise))
        ->send(dialog_id.get_channel_id(), message_id, for_group);
    return {};
  }

  promise.set_value(Unit());
  return it->second;
}

void MessagesManager::on_get_public_message_link(FullMessageId full_message_id, bool for_group, string url,
                                                 string html) {
  LOG_IF(ERROR, url.empty() && html.empty()) << "Receive empty public link for " << full_message_id;
  public_message_links_[for_group][full_message_id] = {std::move(url), std::move(html)};
}

Status MessagesManager::delete_dialog_reply_markup(DialogId dialog_id, MessageId message_id) {
  if (td_->auth_manager_->is_bot()) {
    return Status::Error(6, "Bots can't delete chat reply markup");
  }
  if (!message_id.is_valid()) {
    return Status::Error(6, "Invalid message id specified");
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(6, "Chat not found");
  }
  if (d->reply_markup_message_id != message_id) {
    return Status::OK();
  }

  const Message *message = get_message_force(d, message_id);
  CHECK(message != nullptr);
  CHECK(message->reply_markup != nullptr);

  if (message->reply_markup->type == ReplyMarkup::Type::ForceReply) {
    set_dialog_reply_markup(d, MessageId());
  } else if (message->reply_markup->type == ReplyMarkup::Type::ShowKeyboard) {
    if (!message->reply_markup->is_one_time_keyboard) {
      return Status::Error(6, "Do not need to delete non one-time keyboard");
    }
    if (message->reply_markup->is_personal) {
      message->reply_markup->is_personal = false;
      set_dialog_reply_markup(d, message_id);

      on_message_changed(d, message, "delete_dialog_reply_markup");
    }
  } else {
    // non-bots can't have messages with RemoveKeyboard
    UNREACHABLE();
  }
  return Status::OK();
}

class MessagesManager::SaveDialogDraftMessageOnServerLogEvent {
 public:
  DialogId dialog_id_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id_, parser);
  }
};

Status MessagesManager::set_dialog_draft_message(DialogId dialog_id,
                                                 tl_object_ptr<td_api::draftMessage> &&draft_message) {
  if (td_->auth_manager_->is_bot()) {
    return Status::Error(6, "Bots can't change chat draft message");
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(6, "Chat not found");
  }
  TRY_STATUS(can_send_message(dialog_id));

  unique_ptr<DraftMessage> new_draft_message;
  if (draft_message != nullptr) {
    new_draft_message = make_unique<DraftMessage>();
    new_draft_message->date = G()->unix_time();
    new_draft_message->reply_to_message_id = get_reply_to_message_id(d, MessageId(draft_message->reply_to_message_id_));

    auto input_message_content = std::move(draft_message->input_message_text_);
    if (input_message_content != nullptr) {
      int32 draft_message_content_type = input_message_content->get_id();
      if (draft_message_content_type != td_api::inputMessageText::ID) {
        return Status::Error(5, "Input message content type must be InputMessageText");
      }
      TRY_RESULT(message_content, process_input_message_text(dialog_id, std::move(input_message_content), false, true));
      new_draft_message->input_message_text = std::move(message_content);
    }

    if (!new_draft_message->reply_to_message_id.is_valid() && new_draft_message->input_message_text.text.text.empty()) {
      new_draft_message = nullptr;
    }
  }

  if (update_dialog_draft_message(d, std::move(new_draft_message), false, true)) {
    if (dialog_id.get_type() != DialogType::SecretChat) {
      if (G()->parameters().use_message_db) {
        LOG(INFO) << "Save draft of " << dialog_id << " to binlog";
        SaveDialogDraftMessageOnServerLogEvent logevent;
        logevent.dialog_id_ = dialog_id;
        auto storer = LogEventStorerImpl<SaveDialogDraftMessageOnServerLogEvent>(logevent);
        if (d->save_draft_message_logevent_id == 0) {
          d->save_draft_message_logevent_id = BinlogHelper::add(
              G()->td_db()->get_binlog(), LogEvent::HandlerType::SaveDialogDraftMessageOnServer, storer);
          LOG(INFO) << "Add draft logevent " << d->save_draft_message_logevent_id;
        } else {
          auto new_logevent_id = BinlogHelper::rewrite(G()->td_db()->get_binlog(), d->save_draft_message_logevent_id,
                                                       LogEvent::HandlerType::SaveDialogDraftMessageOnServer, storer);
          LOG(INFO) << "Rewrite draft logevent " << d->save_draft_message_logevent_id << " with " << new_logevent_id;
        }
        d->save_draft_message_logevent_id_generation++;
      }

      pending_draft_message_timeout_.set_timeout_in(dialog_id.get(), d->is_opened ? MIN_SAVE_DRAFT_DELAY : 0);
    }
  }
  return Status::OK();
}

void MessagesManager::save_dialog_draft_message_on_server(DialogId dialog_id) {
  auto d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  Promise<> promise;
  if (d->save_draft_message_logevent_id != 0) {
    promise = PromiseCreator::lambda(
        [actor_id = actor_id(this), dialog_id,
         generation = d->save_draft_message_logevent_id_generation](Result<Unit> result) mutable {
          if (!G()->close_flag()) {
            send_closure(actor_id, &MessagesManager::on_saved_dialog_draft_message, dialog_id, generation);
          }
        });
  }

  // TODO do not send two queries simultaneously or use SequenceDispatcher
  td_->create_handler<SaveDraftMessageQuery>(std::move(promise))->send(dialog_id, d->draft_message);
}

void MessagesManager::on_saved_dialog_draft_message(DialogId dialog_id, uint64 generation) {
  auto d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  LOG(INFO) << "Saved draft in " << dialog_id << " with logevent " << d->save_draft_message_logevent_id;
  if (d->save_draft_message_logevent_id_generation == generation) {
    CHECK(d->save_draft_message_logevent_id != 0);
    LOG(INFO) << "Delete draft logevent " << d->save_draft_message_logevent_id;
    BinlogHelper::erase(G()->td_db()->get_binlog(), d->save_draft_message_logevent_id);
    d->save_draft_message_logevent_id = 0;
  }
}

int32 MessagesManager::get_pinned_dialogs_limit() {
  int32 limit = G()->shared_config().get_option_integer("pinned_chat_count_max");
  if (limit <= 0) {
    const int32 DEFAULT_PINNED_DIALOGS_LIMIT = 5;
    return DEFAULT_PINNED_DIALOGS_LIMIT;
  }
  return limit;
}

vector<DialogId> MessagesManager::remove_secret_chat_dialog_ids(vector<DialogId> dialog_ids) {
  dialog_ids.erase(std::remove_if(dialog_ids.begin(), dialog_ids.end(),
                                  [](DialogId dialog_id) { return dialog_id.get_type() == DialogType::SecretChat; }),
                   dialog_ids.end());
  return dialog_ids;
}

Status MessagesManager::toggle_dialog_is_pinned(DialogId dialog_id, bool is_pinned) {
  if (td_->auth_manager_->is_bot()) {
    return Status::Error(6, "Bots can't change chat pin state");
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(6, "Chat not found");
  }
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    return Status::Error(6, "Can't access the chat");
  }

  bool was_pinned = d->pinned_order != DEFAULT_ORDER;
  if (is_pinned == was_pinned) {
    return Status::OK();
  }

  if (is_pinned) {
    auto pinned_dialog_ids = get_pinned_dialogs();
    auto pinned_dialog_count = pinned_dialog_ids.size();
    auto secret_pinned_dialog_count =
        std::count_if(pinned_dialog_ids.begin(), pinned_dialog_ids.end(),
                      [](DialogId dialog_id) { return dialog_id.get_type() == DialogType::SecretChat; });
    size_t dialog_count = dialog_id.get_type() == DialogType::SecretChat
                              ? secret_pinned_dialog_count
                              : pinned_dialog_count - secret_pinned_dialog_count;

    if (dialog_count >= static_cast<size_t>(get_pinned_dialogs_limit())) {
      return Status::Error(400, "Maximum number of pinned chats exceeded");
    }
  }

  set_dialog_is_pinned(d, is_pinned);
  update_dialog_pos(d, false, "toggle_dialog_is_pinned");

  toggle_dialog_is_pinned_on_server(dialog_id, is_pinned, 0);
  return Status::OK();
}

class MessagesManager::ToggleDialogIsPinnedOnServerLogEvent {
 public:
  DialogId dialog_id_;
  bool is_pinned_;

  template <class StorerT>
  void store(StorerT &storer) const {
    BEGIN_STORE_FLAGS();
    STORE_FLAG(is_pinned_);
    END_STORE_FLAGS();

    td::store(dialog_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    BEGIN_PARSE_FLAGS();
    PARSE_FLAG(is_pinned_);
    END_PARSE_FLAGS();

    td::parse(dialog_id_, parser);
  }
};

void MessagesManager::toggle_dialog_is_pinned_on_server(DialogId dialog_id, bool is_pinned, uint64 logevent_id) {
  if (logevent_id == 0 && G()->parameters().use_message_db) {
    ToggleDialogIsPinnedOnServerLogEvent logevent;
    logevent.dialog_id_ = dialog_id;
    logevent.is_pinned_ = is_pinned;

    auto storer = LogEventStorerImpl<ToggleDialogIsPinnedOnServerLogEvent>(logevent);
    logevent_id =
        BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::ToggleDialogIsPinnedOnServer, storer);
  }

  Promise<> promise;
  if (logevent_id != 0) {
    promise = PromiseCreator::lambda([logevent_id](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }
    });
  }

  td_->create_handler<ToggleDialogPinQuery>(std::move(promise))->send(dialog_id, is_pinned);
}

Status MessagesManager::set_pinned_dialogs(vector<DialogId> dialog_ids) {
  if (td_->auth_manager_->is_bot()) {
    return Status::Error(6, "Bots can't reorder pinned chats");
  }

  int32 dialog_count = 0;
  int32 secret_dialog_count = 0;
  auto dialog_count_limit = get_pinned_dialogs_limit();
  for (auto dialog_id : dialog_ids) {
    Dialog *d = get_dialog_force(dialog_id);
    if (d == nullptr) {
      return Status::Error(6, "Chat not found");
    }
    if (!have_input_peer(dialog_id, AccessRights::Read)) {
      return Status::Error(6, "Can't access the chat");
    }
    if (dialog_id.get_type() == DialogType::SecretChat) {
      secret_dialog_count++;
    } else {
      dialog_count++;
    }

    if (dialog_count > dialog_count_limit || secret_dialog_count > dialog_count_limit) {
      return Status::Error(400, "Maximum number of pinned chats exceeded");
    }
  }
  std::unordered_set<DialogId, DialogIdHash> new_pinned_dialog_ids(dialog_ids.begin(), dialog_ids.end());
  if (new_pinned_dialog_ids.size() != dialog_ids.size()) {
    return Status::Error(400, "Duplicate chats in the list of pinned chats");
  }

  auto pinned_dialog_ids = get_pinned_dialogs();
  if (pinned_dialog_ids == dialog_ids) {
    return Status::OK();
  }
  LOG(INFO) << "Reorder pinned chats order from " << format::as_array(pinned_dialog_ids) << " to "
            << format::as_array(dialog_ids);

  auto server_old_dialog_ids = remove_secret_chat_dialog_ids(pinned_dialog_ids);
  auto server_new_dialog_ids = remove_secret_chat_dialog_ids(dialog_ids);

  std::reverse(pinned_dialog_ids.begin(), pinned_dialog_ids.end());
  std::reverse(dialog_ids.begin(), dialog_ids.end());

  std::unordered_set<DialogId, DialogIdHash> old_pinned_dialog_ids(pinned_dialog_ids.begin(), pinned_dialog_ids.end());
  auto old_it = pinned_dialog_ids.begin();
  for (auto dialog_id : dialog_ids) {
    old_pinned_dialog_ids.erase(dialog_id);
    while (old_it < pinned_dialog_ids.end()) {
      if (*old_it == dialog_id) {
        break;
      }
      old_it++;
    }
    if (old_it < pinned_dialog_ids.end()) {
      // leave dialog where it is
      continue;
    }
    set_dialog_is_pinned(dialog_id, true);
  }
  for (auto dialog_id : old_pinned_dialog_ids) {
    set_dialog_is_pinned(dialog_id, false);
  }

  if (server_old_dialog_ids != server_new_dialog_ids) {
    reorder_pinned_dialogs_on_server(server_new_dialog_ids, 0);
  }
  return Status::OK();
}

class MessagesManager::ReorderPinnedDialogsOnServerLogEvent {
 public:
  vector<DialogId> dialog_ids_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_ids_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_ids_, parser);
  }
};

void MessagesManager::reorder_pinned_dialogs_on_server(const vector<DialogId> &dialog_ids, uint64 logevent_id) {
  if (logevent_id == 0 && G()->parameters().use_message_db) {
    ReorderPinnedDialogsOnServerLogEvent logevent;
    logevent.dialog_ids_ = dialog_ids;

    auto storer = LogEventStorerImpl<ReorderPinnedDialogsOnServerLogEvent>(logevent);
    logevent_id =
        BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::ReorderPinnedDialogsOnServer, storer);
  }

  Promise<> promise;
  if (logevent_id != 0) {
    promise = PromiseCreator::lambda([logevent_id](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }
    });
  }

  td_->create_handler<ReorderPinnedDialogsQuery>(std::move(promise))->send(dialog_ids);
}

Status MessagesManager::set_dialog_client_data(DialogId dialog_id, string &&client_data) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(6, "Chat not found");
  }

  d->client_data = std::move(client_data);
  on_dialog_updated(d->dialog_id, "set_dialog_client_data");
  return Status::OK();
}

void MessagesManager::create_dialog(DialogId dialog_id, bool force, Promise<Unit> &&promise) {
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    if (!have_dialog_info_force(dialog_id)) {
      return promise.set_error(Status::Error(6, "Chat info not found"));
    }
    if (!have_input_peer(dialog_id, AccessRights::Read)) {
      return promise.set_error(Status::Error(6, "Can't access the chat"));
    }
  }

  if (force || td_->auth_manager_->is_bot() || dialog_id.get_type() == DialogType::SecretChat) {
    force_create_dialog(dialog_id, "create dialog");
  } else {
    const Dialog *d = get_dialog_force(dialog_id);
    if (d == nullptr || !d->notification_settings.is_synchronized) {
      return send_get_dialog_query(dialog_id, std::move(promise));
    }
  }

  promise.set_value(Unit());
}

DialogId MessagesManager::create_new_group_chat(const vector<UserId> &user_ids, const string &title, int64 &random_id,
                                                Promise<Unit> &&promise) {
  LOG(INFO) << "Trying to create group chat \"" << title << "\" with members " << format::as_array(user_ids);

  if (random_id != 0) {
    // request has already been sent before
    auto it = created_dialogs_.find(random_id);
    CHECK(it != created_dialogs_.end());
    auto dialog_id = it->second;
    CHECK(dialog_id.get_type() == DialogType::Chat);
    CHECK(have_dialog(dialog_id));

    created_dialogs_.erase(it);

    // set default notification settings to newly created chat
    on_update_notify_settings(dialog_id.get(),
                              make_tl_object<telegram_api::peerNotifySettings>(1, true, false, 0, "default"));

    promise.set_value(Unit());
    return dialog_id;
  }

  if (user_ids.empty()) {
    promise.set_error(Status::Error(3, "Too few users to create basic group chat"));
    return DialogId();
  }

  auto new_title = clean_name(title, MAX_NAME_LENGTH);
  if (new_title.empty()) {
    promise.set_error(Status::Error(3, "Title can't be empty"));
    return DialogId();
  }

  vector<tl_object_ptr<telegram_api::InputUser>> input_users;
  for (auto user_id : user_ids) {
    auto input_user = td_->contacts_manager_->get_input_user(user_id);
    if (input_user == nullptr) {
      promise.set_error(Status::Error(3, "User not found"));
      return DialogId();
    }
    input_users.push_back(std::move(input_user));
  }

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || created_dialogs_.find(random_id) != created_dialogs_.end());
  created_dialogs_[random_id];  // reserve place for result

  td_->create_handler<CreateChatQuery>(std::move(promise))->send(std::move(input_users), new_title, random_id);
  return DialogId();
}

DialogId MessagesManager::create_new_channel_chat(const string &title, bool is_megagroup, const string &description,
                                                  int64 &random_id, Promise<Unit> &&promise) {
  LOG(INFO) << "Trying to create " << (is_megagroup ? "supergroup" : "broadcast") << " with title \"" << title
            << "\" and description \"" << description << "\"";

  if (random_id != 0) {
    // request has already been sent before
    auto it = created_dialogs_.find(random_id);
    CHECK(it != created_dialogs_.end());
    auto dialog_id = it->second;
    CHECK(dialog_id.get_type() == DialogType::Channel);
    CHECK(have_dialog(dialog_id));

    created_dialogs_.erase(it);

    // set default notification settings to newly created chat
    on_update_notify_settings(dialog_id.get(),
                              make_tl_object<telegram_api::peerNotifySettings>(1, true, false, 0, "default"));

    promise.set_value(Unit());
    return dialog_id;
  }

  auto new_title = clean_name(title, MAX_NAME_LENGTH);
  if (new_title.empty()) {
    promise.set_error(Status::Error(3, "Title can't be empty"));
    return DialogId();
  }

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || created_dialogs_.find(random_id) != created_dialogs_.end());
  created_dialogs_[random_id];  // reserve place for result

  td_->create_handler<CreateChannelQuery>(std::move(promise))
      ->send(new_title, is_megagroup, strip_empty_characters(description, MAX_NAME_LENGTH), random_id);
  return DialogId();
}

void MessagesManager::create_new_secret_chat(UserId user_id, Promise<SecretChatId> &&promise) {
  auto user_base = td_->contacts_manager_->get_input_user(user_id);
  if (user_base == nullptr || user_base->get_id() != telegram_api::inputUser::ID) {
    return promise.set_error(Status::Error(6, "User not found"));
  }
  auto user = move_tl_object_as<telegram_api::inputUser>(user_base);

  send_closure(G()->secret_chats_manager(), &SecretChatsManager::create_chat, user->user_id_, user->access_hash_,
               std::move(promise));
}

DialogId MessagesManager::migrate_dialog_to_megagroup(DialogId dialog_id, Promise<Unit> &&promise) {
  LOG(INFO) << "Trying to convert " << dialog_id << " to supergroup";

  if (dialog_id.get_type() != DialogType::Chat) {
    promise.set_error(Status::Error(3, "Only basic group chats can be converted to supergroup"));
    return DialogId();
  }

  auto channel_id = td_->contacts_manager_->migrate_chat_to_megagroup(dialog_id.get_chat_id(), promise);
  if (!channel_id.is_valid()) {
    return DialogId();
  }

  if (!td_->contacts_manager_->have_channel(channel_id)) {
    LOG(ERROR) << "Can't find info about supergroup to which the group has migrated";
    promise.set_error(Status::Error(6, "Supergroup is not found"));
    return DialogId();
  }

  auto new_dialog_id = DialogId(channel_id);
  Dialog *d = get_dialog_force(new_dialog_id);
  if (d == nullptr) {
    d = add_dialog(new_dialog_id);
    if (d->pts == 0) {
      d->pts = 1;
      if (is_debug_message_op_enabled()) {
        d->debug_message_op.emplace_back(Dialog::MessageOp::SetPts, MessageId(), d->pts, false, false, false,
                                         "migrate");
      }
    }
    update_dialog_pos(d, false, "migrate_dialog_to_megagroup");
  }

  promise.set_value(Unit());
  return new_dialog_id;
}

Status MessagesManager::open_dialog(DialogId dialog_id) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(3, "Chat not found");
  }

  open_dialog(d);
  return Status::OK();
}

Status MessagesManager::close_dialog(DialogId dialog_id) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(3, "Chat not found");
  }

  close_dialog(d);
  return Status::OK();
}

Status MessagesManager::view_messages(DialogId dialog_id, const vector<MessageId> &message_ids, bool force_read) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(3, "Chat not found");
  }
  for (auto message_id : message_ids) {
    if (!message_id.is_valid()) {
      return Status::Error(3, "Invalid message identifier");
    }
  }
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    return Status::Error(5, "Can't access the chat");
  }

  bool need_read = force_read || d->is_opened;
  bool is_secret = dialog_id.get_type() == DialogType::SecretChat;
  MessageId max_incoming_message_id;
  MessageId max_message_id;
  vector<MessageId> read_content_message_ids;
  for (auto message_id : message_ids) {
    auto message = get_message_force(d, message_id);
    if (message != nullptr) {
      if (message_id.is_server() && message->views > 0) {
        d->pending_viewed_message_ids.insert(message_id);
      }

      if (!message_id.is_yet_unsent() && message_id.get() > max_incoming_message_id.get()) {
        if (!message->is_outgoing && (message_id.is_server() || is_secret)) {
          max_incoming_message_id = message_id;
        }
        if (message_id.get() > max_message_id.get()) {
          max_message_id = message_id;
        }
      }

      if (need_read) {
        auto message_content_type = message->content->get_id();
        if (message_content_type != MessageVoiceNote::ID && message_content_type != MessageVideoNote::ID &&
            update_message_contains_unread_mention(d, message, false, "view_messages")) {
          CHECK(message_id.is_server());
          read_content_message_ids.push_back(message_id);
          on_message_changed(d, message, "view_messages");
        }
      }
    }
  }
  if (!d->pending_viewed_message_ids.empty()) {
    pending_message_views_timeout_.add_timeout_in(dialog_id.get(), MAX_MESSAGE_VIEW_DELAY);
    d->increment_view_counter |= d->is_opened;
  }
  if (!read_content_message_ids.empty()) {
    read_message_contents_on_server(dialog_id, std::move(read_content_message_ids), 0);
  }

  if (need_read && max_message_id.get() > d->last_read_inbox_message_id.get()) {
    MessageId last_read_message_id = d->last_message_id;
    if (!last_read_message_id.is_valid() || last_read_message_id.is_yet_unsent()) {
      if (max_message_id.get() <= d->last_new_message_id.get()) {
        last_read_message_id = d->last_new_message_id;
      } else {
        last_read_message_id = max_message_id;
      }
    }
    read_history_inbox(d->dialog_id, last_read_message_id, -1, "view_messages");

    if (d->last_new_message_id.is_valid()) {
      if (!d->is_last_read_inbox_message_id_inited) {
        // don't know last read inbox message, read history on the server just in case
        read_history_on_server(d->dialog_id, d->last_new_message_id, true, 0);
      } else {
        if (max_incoming_message_id.get() <= d->last_read_inbox_message_id.get()) {
          MessagesConstIterator p(d, d->last_message_id);
          while (*p != nullptr && ((*p)->is_outgoing || !((*p)->message_id.is_server() || is_secret)) &&
                 (*p)->message_id.get() > d->last_read_inbox_message_id.get()) {
            --p;
          }
          if (*p != nullptr && !(*p)->is_outgoing && ((*p)->message_id.is_server() || is_secret)) {
            max_incoming_message_id = (*p)->message_id;
          }
        }

        if (max_incoming_message_id.get() > d->last_read_inbox_message_id.get()) {
          LOG_IF(ERROR, d->server_unread_count + d->local_unread_count == 0) << "Nave no unread messages";
          read_history_on_server(d->dialog_id, d->last_new_message_id, false,
                                 0);  // TODO can read messages only up to max_incoming_message_id
        } else {
          // can't find last incoming message, read history on the server just in case
          read_history_on_server(d->dialog_id, d->last_new_message_id, true, 0);
        }
      }
    }
  }

  return Status::OK();
}

Status MessagesManager::open_message_content(FullMessageId full_message_id) {
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(3, "Chat not found");
  }

  auto message_id = full_message_id.get_message_id();
  auto message = get_message_force(d, message_id);
  if (message == nullptr) {
    return Status::Error(4, "Message not found");
  }

  if (message_id.is_yet_unsent() || message->is_outgoing) {
    return Status::OK();
  }

  if (read_message_content(d, message, true, "open_message_content") &&
      (message_id.is_server() || dialog_id.get_type() == DialogType::SecretChat)) {
    read_message_contents_on_server(dialog_id, {message_id}, 0);
  }

  return Status::OK();
}

class MessagesManager::ReadMessageContentsOnServerLogEvent {
 public:
  DialogId dialog_id_;
  vector<MessageId> message_ids_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id_, storer);
    td::store(message_ids_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id_, parser);
    td::parse(message_ids_, parser);
  }
};

void MessagesManager::read_message_contents_on_server(DialogId dialog_id, vector<MessageId> message_ids,
                                                      uint64 logevent_id) {
  CHECK(!message_ids.empty());

  LOG(INFO) << "Read contents of " << format::as_array(message_ids) << " in " << dialog_id << " on server";

  if (logevent_id == 0 && G()->parameters().use_message_db) {
    ReadMessageContentsOnServerLogEvent logevent;
    logevent.dialog_id_ = dialog_id;
    logevent.message_ids_ = message_ids;

    auto storer = LogEventStorerImpl<ReadMessageContentsOnServerLogEvent>(logevent);
    logevent_id =
        BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::ReadMessageContentsOnServer, storer);
  }

  Promise<> promise;
  if (logevent_id != 0) {
    promise = PromiseCreator::lambda([logevent_id](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }
    });
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      td_->create_handler<ReadMessagesContentsQuery>(std::move(promise))->send(std::move(message_ids));
      break;
    case DialogType::Channel:
      td_->create_handler<ReadChannelMessagesContentsQuery>(std::move(promise))
          ->send(dialog_id.get_channel_id(), std::move(message_ids));
      break;
    case DialogType::SecretChat:
      CHECK(message_ids.size() == 1);
      for (auto message_id : message_ids) {
        auto m = get_message_force({dialog_id, message_id});
        if (m != nullptr) {
          send_closure(G()->secret_chats_manager(), &SecretChatsManager::send_open_message,
                       dialog_id.get_secret_chat_id(), m->random_id, std::move(promise));
        }
      }
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void MessagesManager::open_dialog(Dialog *d) {
  if (d->is_opened || !have_input_peer(d->dialog_id, AccessRights::Read)) {
    return;
  }
  d->is_opened = true;

  auto min_message_id = MessageId(ServerMessageId(1)).get();
  if (d->last_message_id == MessageId() && d->last_read_outbox_message_id.get() < min_message_id &&
      d->messages != nullptr && d->messages->message_id.get() < min_message_id) {
    Message *m = d->messages.get();
    while (m->right != nullptr) {
      m = m->right.get();
    }
    if (m->message_id.get() < min_message_id) {
      read_history_inbox(d->dialog_id, m->message_id, -1, "open_dialog");
    }
  }

  LOG(INFO) << "Cancel unload timeout for " << d->dialog_id;
  pending_unload_dialog_timeout_.cancel_timeout(d->dialog_id.get());

  switch (d->dialog_id.get_type()) {
    case DialogType::User:
      break;
    case DialogType::Chat: {
      auto chat_id = d->dialog_id.get_chat_id();
      td_->contacts_manager_->get_chat_full(chat_id, Promise<Unit>());
      break;
    }
    case DialogType::Channel:
      get_channel_difference(d->dialog_id, d->pts, true, "open_dialog");
      break;
    case DialogType::SecretChat:
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void MessagesManager::close_dialog(Dialog *d) {
  if (!d->is_opened) {
    return;
  }
  d->is_opened = false;

  if (have_input_peer(d->dialog_id, AccessRights::Write)) {
    if (pending_draft_message_timeout_.has_timeout(d->dialog_id.get())) {
      pending_draft_message_timeout_.set_timeout_in(d->dialog_id.get(), 0.0);
    }
  } else {
    pending_draft_message_timeout_.cancel_timeout(d->dialog_id.get());
  }

  if (have_input_peer(d->dialog_id, AccessRights::Read)) {
    if (pending_message_views_timeout_.has_timeout(d->dialog_id.get())) {
      pending_message_views_timeout_.set_timeout_in(d->dialog_id.get(), 0.0);
    }
  } else {
    pending_message_views_timeout_.cancel_timeout(d->dialog_id.get());
    d->pending_viewed_message_ids.clear();
    d->increment_view_counter = false;
  }

  if (is_message_unload_enabled()) {
    LOG(INFO) << "Schedule unload of " << d->dialog_id;
    pending_unload_dialog_timeout_.set_timeout_in(d->dialog_id.get(), DIALOG_UNLOAD_DELAY);
  }

  switch (d->dialog_id.get_type()) {
    case DialogType::User:
      break;
    case DialogType::Chat:
      break;
    case DialogType::Channel:
      channel_get_difference_timeout_.cancel_timeout(d->dialog_id.get());
      break;
    case DialogType::SecretChat:
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

tl_object_ptr<td_api::inputMessageText> MessagesManager::get_input_message_text_object(
    const InputMessageText &input_message_text) const {
  return make_tl_object<td_api::inputMessageText>(get_formatted_text_object(input_message_text.text),
                                                  input_message_text.disable_web_page_preview,
                                                  input_message_text.clear_draft);
}

tl_object_ptr<td_api::draftMessage> MessagesManager::get_draft_message_object(
    const unique_ptr<DraftMessage> &draft_message) const {
  if (draft_message == nullptr) {
    return nullptr;
  }
  return make_tl_object<td_api::draftMessage>(draft_message->reply_to_message_id.get(),
                                              get_input_message_text_object(draft_message->input_message_text));
}

tl_object_ptr<td_api::ChatType> MessagesManager::get_chat_type_object(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      return make_tl_object<td_api::chatTypePrivate>(
          td_->contacts_manager_->get_user_id_object(dialog_id.get_user_id(), "chatTypePrivate"));
    case DialogType::Chat:
      return make_tl_object<td_api::chatTypeBasicGroup>(
          td_->contacts_manager_->get_basic_group_id_object(dialog_id.get_chat_id(), "chatTypeBasicGroup"));
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      auto channel_type = td_->contacts_manager_->get_channel_type(channel_id);
      return make_tl_object<td_api::chatTypeSupergroup>(
          td_->contacts_manager_->get_supergroup_id_object(channel_id, "chatTypeSupergroup"),
          channel_type != ChannelType::Megagroup);
    }
    case DialogType::SecretChat: {
      auto secret_chat_id = dialog_id.get_secret_chat_id();
      auto user_id = td_->contacts_manager_->get_secret_chat_user_id(secret_chat_id);
      return make_tl_object<td_api::chatTypeSecret>(
          td_->contacts_manager_->get_secret_chat_id_object(secret_chat_id, "chatTypeSecret"),
          td_->contacts_manager_->get_user_id_object(user_id, "chatTypeSecret"));
    }
    case DialogType::None:
    default:
      UNREACHABLE();
      return nullptr;
  }
}

tl_object_ptr<td_api::chat> MessagesManager::get_chat_object(const Dialog *d) {
  if (!td_->auth_manager_->is_bot()) {
    if (d->last_new_message_id.is_valid()) {
      // if (preloaded_dialogs_++ < 0) {
      //   preload_older_messages(d, d->last_new_message_id);
      // }
    }
    if (!d->notification_settings.is_synchronized && d->dialog_id.get_type() != DialogType::SecretChat &&
        have_input_peer(d->dialog_id, AccessRights::Read)) {
      // asynchronously get dialog from the server
      send_get_dialog_query(d->dialog_id, Auto());
    }
  }

  return make_tl_object<td_api::chat>(
      d->dialog_id.get(), get_chat_type_object(d->dialog_id), get_dialog_title(d->dialog_id),
      get_chat_photo_object(td_->file_manager_.get(), get_dialog_photo(d->dialog_id)),
      get_message_object(d->dialog_id, get_message(d, d->last_message_id)),
      DialogDate(d->order, d->dialog_id) <= last_dialog_date_ ? d->order : 0, d->pinned_order != DEFAULT_ORDER,
      can_report_dialog(d->dialog_id), d->server_unread_count + d->local_unread_count,
      d->last_read_inbox_message_id.get(), d->last_read_outbox_message_id.get(), d->unread_mention_count,
      get_notification_settings_object(&d->notification_settings), d->reply_markup_message_id.get(),
      get_draft_message_object(d->draft_message), d->client_data);
}

tl_object_ptr<td_api::chat> MessagesManager::get_chat_object(DialogId dialog_id) {
  auto d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  return get_chat_object(d);
}

tl_object_ptr<td_api::chats> MessagesManager::get_chats_object(const vector<DialogId> &dialogs) {
  return td_api::make_object<td_api::chats>(transform(dialogs, [](DialogId dialog_id) { return dialog_id.get(); }));
}

tl_object_ptr<td_api::NotificationSettingsScope> MessagesManager::get_notification_settings_scope_object(
    NotificationSettingsScope scope) {
  switch (scope) {
    case NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS:
      return make_tl_object<td_api::notificationSettingsScopePrivateChats>();
    case NOTIFICATION_SETTINGS_FOR_GROUP_CHATS:
      return make_tl_object<td_api::notificationSettingsScopeBasicGroupChats>();
    case NOTIFICATION_SETTINGS_FOR_ALL_CHATS:
      return make_tl_object<td_api::notificationSettingsScopeAllChats>();
    default:
      return make_tl_object<td_api::notificationSettingsScopeChat>(scope);
  }
}

tl_object_ptr<td_api::notificationSettings> MessagesManager::get_notification_settings_object(
    const NotificationSettings *notification_settings) {
  return make_tl_object<td_api::notificationSettings>(max(0, notification_settings->mute_until - G()->unix_time()),
                                                      notification_settings->sound,
                                                      notification_settings->show_preview);
}

const NotificationSettings *MessagesManager::get_dialog_notification_settings(const Dialog *d,
                                                                              DialogId dialog_id) const {
  if (d != nullptr &&
      d->notification_settings.is_synchronized) {  // TODO this is wrong check for initialized notification settings
    return &d->notification_settings;
  }

  const NotificationSettings *notification_settings = nullptr;
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::SecretChat:
      notification_settings = &users_notification_settings_;
      break;
    case DialogType::Chat:
      notification_settings = &chats_notification_settings_;
      break;
    case DialogType::Channel: {
      ChannelId channel_id = dialog_id.get_channel_id();
      auto channel_type = td_->contacts_manager_->get_channel_type(channel_id);
      if (channel_type == ChannelType::Megagroup) {
        return nullptr;
      }
      notification_settings = &chats_notification_settings_;
      break;
    }
    case DialogType::None:
    default:
      UNREACHABLE();
      return nullptr;
  }
  if (notification_settings->is_synchronized) {
    return notification_settings;
  }
  return &dialogs_notification_settings_;
}

const NotificationSettings *MessagesManager::get_notification_settings(NotificationSettingsScope scope,
                                                                       Promise<Unit> &&promise) {
  const NotificationSettings *notification_settings = get_notification_settings(scope, true);
  if (notification_settings == nullptr) {
    promise.set_error(Status::Error(3, "Chat not found"));
    return nullptr;
  }

  if (!notification_settings->is_synchronized && get_notification_settings(scope, false) != nullptr &&
      !td_->auth_manager_->is_bot()) {
    td_->create_handler<GetNotifySettingsQuery>(std::move(promise))->send(scope);
    return nullptr;
  }

  promise.set_value(Unit());
  return notification_settings;
}

NotificationSettings *MessagesManager::get_notification_settings(NotificationSettingsScope scope, bool force) {
  switch (scope) {
    case NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS:
      return &users_notification_settings_;
    case NOTIFICATION_SETTINGS_FOR_GROUP_CHATS:
      return &chats_notification_settings_;
    case NOTIFICATION_SETTINGS_FOR_ALL_CHATS:
      return &dialogs_notification_settings_;
    default: {
      DialogId dialog_id(scope);
      auto dialog = get_dialog_force(dialog_id);
      if (dialog == nullptr) {
        return nullptr;
      }
      if (!force && !have_input_peer(dialog_id, AccessRights::Read)) {
        return nullptr;
      }
      return &dialog->notification_settings;
    }
  }
}

tl_object_ptr<telegram_api::InputNotifyPeer> MessagesManager::get_input_notify_peer(
    NotificationSettingsScope scope) const {
  switch (scope) {
    case NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS:
      return make_tl_object<telegram_api::inputNotifyUsers>();
    case NOTIFICATION_SETTINGS_FOR_GROUP_CHATS:
      return make_tl_object<telegram_api::inputNotifyChats>();
    case NOTIFICATION_SETTINGS_FOR_ALL_CHATS:
      return make_tl_object<telegram_api::inputNotifyAll>();
    default: {
      DialogId dialog_id(scope);
      if (get_dialog(dialog_id) == nullptr) {
        return nullptr;
      }
      auto input_peer = get_input_peer(dialog_id, AccessRights::Read);
      if (input_peer == nullptr) {
        return nullptr;
      }
      return make_tl_object<telegram_api::inputNotifyPeer>(std::move(input_peer));
    }
  }
}

Status MessagesManager::set_notification_settings(NotificationSettingsScope scope,
                                                  tl_object_ptr<td_api::notificationSettings> &&notification_settings) {
  if (notification_settings == nullptr) {
    return Status::Error(400, "New notification settings must not be empty");
  }
  if (!clean_input_string(notification_settings->sound_)) {
    return Status::Error(400, "Notification settings sound must be encoded in UTF-8");
  }

  auto current_settings = get_notification_settings(scope, false);
  if (current_settings == nullptr) {
    return Status::Error(6, "Wrong chat identifier specified");
  }

  int32 current_time = G()->unix_time();
  if (notification_settings->mute_for_ > std::numeric_limits<int32>::max() - current_time) {
    notification_settings->mute_for_ = std::numeric_limits<int32>::max() - current_time;
  }

  int32 mute_until;
  if (notification_settings->mute_for_ <= 0) {
    mute_until = 0;
  } else {
    mute_until = notification_settings->mute_for_ + current_time;
  }

  NotificationSettings new_settings(mute_until, std::move(notification_settings->sound_),
                                    notification_settings->show_preview_, current_settings->silent_send_message);

  if (update_notification_settings(scope, current_settings, new_settings)) {
    td_->create_handler<UpdateNotifySettingsQuery>()->send(scope, new_settings);
  }
  return Status::OK();
}

void MessagesManager::reset_all_notification_settings() {
  NotificationSettings new_settings(0, "default", true, false);
  NotificationSettings new_megagroup_settings(std::numeric_limits<int32>::max(), "default", true, false);

  update_notification_settings(NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS, &users_notification_settings_, new_settings);
  update_notification_settings(NOTIFICATION_SETTINGS_FOR_GROUP_CHATS, &chats_notification_settings_, new_settings);
  update_notification_settings(NOTIFICATION_SETTINGS_FOR_ALL_CHATS, &dialogs_notification_settings_, new_settings);

  for (auto &dialog : dialogs_) {
    Dialog *d = dialog.second.get();
    auto dialog_id = d->dialog_id;
    bool is_megagroup = dialog_id.get_type() == DialogType::Channel &&
                        td_->contacts_manager_->get_channel_type(dialog_id.get_channel_id()) == ChannelType::Megagroup;

    if (is_megagroup) {
      update_notification_settings(NotificationSettingsScope(dialog_id.get()), &d->notification_settings,
                                   new_megagroup_settings);
    } else {
      update_notification_settings(NotificationSettingsScope(dialog_id.get()), &d->notification_settings, new_settings);
    }
  }
  td_->create_handler<ResetNotifySettingsQuery>()->send();
}

unique_ptr<DraftMessage> MessagesManager::get_draft_message(
    ContactsManager *contacts_manager, tl_object_ptr<telegram_api::DraftMessage> &&draft_message_ptr) {
  if (draft_message_ptr == nullptr) {
    return nullptr;
  }
  auto constructor_id = draft_message_ptr->get_id();
  switch (constructor_id) {
    case telegram_api::draftMessageEmpty::ID:
      return nullptr;
    case telegram_api::draftMessage::ID: {
      auto draft = move_tl_object_as<telegram_api::draftMessage>(draft_message_ptr);
      auto flags = draft->flags_;
      auto result = make_unique<DraftMessage>();
      result->date = draft->date_;
      if ((flags & SEND_MESSAGE_FLAG_IS_REPLY) != 0) {
        result->reply_to_message_id = MessageId(ServerMessageId(draft->reply_to_msg_id_));
        if (!result->reply_to_message_id.is_valid()) {
          LOG(ERROR) << "Receive " << result->reply_to_message_id << " as reply_to_message_id in the draft";
          result->reply_to_message_id = MessageId();
        }
      }

      auto entities = get_message_entities(contacts_manager, std::move(draft->entities_), "draftMessage");
      auto status = fix_formatted_text(draft->message_, entities, true, true, true, true);
      if (status.is_error()) {
        LOG(ERROR) << "Receive error " << status << " while parsing draft " << draft->message_;
        if (!clean_input_string(draft->message_)) {
          draft->message_.clear();
        }
        entities.clear();
      }
      result->input_message_text.text = FormattedText{std::move(draft->message_), std::move(entities)};
      result->input_message_text.disable_web_page_preview = (flags & SEND_MESSAGE_FLAG_DISABLE_WEB_PAGE_PREVIEW) != 0;
      result->input_message_text.clear_draft = false;

      return result;
    }
    default:
      UNREACHABLE();
      return nullptr;
  }
}

tl_object_ptr<td_api::messages> MessagesManager::get_dialog_history(DialogId dialog_id, MessageId from_message_id,
                                                                    int32 offset, int32 limit, int left_tries,
                                                                    bool only_local, Promise<Unit> &&promise) {
  if (limit <= 0) {
    promise.set_error(Status::Error(3, "Parameter limit must be positive"));
    return nullptr;
  }
  if (limit > MAX_GET_HISTORY) {
    limit = MAX_GET_HISTORY;
  }
  if (limit <= -offset) {
    promise.set_error(Status::Error(5, "Parameter limit must be greater than -offset"));
    return nullptr;
  }
  if (offset > 0) {
    promise.set_error(Status::Error(5, "Parameter offset must be non-positive"));
    return nullptr;
  }

  if (from_message_id == MessageId() || from_message_id.get() > MessageId::max().get()) {
    from_message_id = MessageId::max();
  }
  if (!from_message_id.is_valid()) {
    promise.set_error(Status::Error(3, "Invalid value of parameter from_message_id specified"));
    return nullptr;
  }

  const Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(6, "Chat not found"));
    return nullptr;
  }
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    promise.set_error(Status::Error(5, "Can't access the chat"));
    return nullptr;
  }

  LOG(INFO) << "Get " << (only_local ? "local " : "") << "history in " << dialog_id << " from " << from_message_id
            << " with offset " << offset << " and limit " << limit << ", " << left_tries
            << " tries left. Last read inbox message is " << d->last_read_inbox_message_id
            << ", last read outbox message is " << d->last_read_outbox_message_id
            << ", have_full_history = " << d->have_full_history;

  MessagesConstIterator p(d, from_message_id);
  LOG(DEBUG) << "Iterator points to " << (*p ? (*p)->message_id : MessageId());
  bool from_the_end = (d->last_message_id != MessageId() && from_message_id.get() > d->last_message_id.get()) ||
                      from_message_id.get() >= MessageId::max().get();

  if (from_the_end) {
    limit += offset;
    offset = 0;
    if (d->last_message_id == MessageId()) {
      p = MessagesConstIterator();
    }
  } else {
    bool have_a_gap = false;
    if (*p == nullptr) {
      // there is no gap if from_message_id is less than first message in the dialog
      if (left_tries == 0 && d->messages != nullptr && offset < 0) {
        Message *cur = d->messages.get();
        while (cur->left != nullptr) {
          cur = cur->left.get();
        }
        CHECK(cur->message_id.get() > from_message_id.get());
        from_message_id = cur->message_id;
        p = MessagesConstIterator(d, from_message_id);
      } else {
        have_a_gap = true;
      }
    } else if ((*p)->message_id != from_message_id) {
      CHECK((*p)->message_id.get() < from_message_id.get());
      if (!(*p)->have_next &&
          (d->last_message_id == MessageId() || (*p)->message_id.get() < d->last_message_id.get())) {
        have_a_gap = true;
      }
    }

    if (have_a_gap) {
      LOG(INFO) << "Have a gap near message to get chat history from";
      p = MessagesConstIterator();
    }
    if (*p != nullptr && (*p)->message_id == from_message_id) {
      if (offset < 0) {
        offset++;
      } else {
        --p;
      }
    }

    while (*p != nullptr && offset < 0) {
      ++p;
      if (*p) {
        ++offset;
        from_message_id = (*p)->message_id;
      }
    }

    if (offset < 0 && ((d->last_message_id != MessageId() && from_message_id.get() >= d->last_message_id.get()) ||
                       (!have_a_gap && left_tries == 0))) {
      CHECK(!have_a_gap);
      limit += offset;
      offset = 0;
      p = MessagesConstIterator(d, from_message_id);
    }

    if (!have_a_gap && offset < 0) {
      offset--;
    }
  }

  LOG(INFO) << "Iterator after applying offset points to " << (*p ? (*p)->message_id : MessageId())
            << ", offset = " << offset << ", limit = " << limit << ", from_the_end = " << from_the_end;
  vector<tl_object_ptr<td_api::message>> messages;
  if (*p != nullptr && offset == 0) {
    while (*p != nullptr && limit-- > 0) {
      messages.push_back(get_message_object(dialog_id, *p));
      from_message_id = (*p)->message_id;
      --p;
    }
  }

  if (messages.size()) {
    // maybe need some messages
    CHECK(offset == 0);
    preload_newer_messages(d, MessageId(messages[0]->id_));
    preload_older_messages(d, MessageId(messages.back()->id_));
  } else if (limit > 0 && (/*d->first_remote_message_id != -1 && */ left_tries != 0)) {
    // there can be more messages on the server, need to load them
    if (from_the_end) {
      from_message_id = MessageId();
    }
    send_closure_later(actor_id(this), &MessagesManager::load_messages, d->dialog_id, from_message_id, offset, limit,
                       left_tries, only_local, std::move(promise));
    return nullptr;
  }

  LOG(INFO) << "Return " << messages.size() << " messages in result to getChatHistory";
  promise.set_value(Unit());                            // can send some messages
  return get_messages_object(-1, std::move(messages));  // TODO return real total_count of messages in the dialog
}

class MessagesManager::ReadHistoryOnServerLogEvent {
 public:
  DialogId dialog_id_;
  MessageId max_message_id_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id_, storer);
    td::store(max_message_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id_, parser);
    td::parse(max_message_id_, parser);
  }
};

void MessagesManager::read_history_on_server(DialogId dialog_id, MessageId max_message_id, bool allow_error,
                                             uint64 logevent_id) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  LOG(INFO) << "Read history in " << dialog_id << " on server up to " << max_message_id;

  if (logevent_id == 0 && G()->parameters().use_message_db) {
    ReadHistoryOnServerLogEvent logevent;
    logevent.dialog_id_ = dialog_id;
    logevent.max_message_id_ = max_message_id;

    auto storer = LogEventStorerImpl<ReadHistoryOnServerLogEvent>(logevent);
    logevent_id = BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::ReadHistoryOnServer, storer);
  }

  Promise<> promise;
  if (logevent_id != 0) {
    promise = PromiseCreator::lambda([logevent_id](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }
    });
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      td_->create_handler<ReadHistoryQuery>(std::move(promise))->send(dialog_id, max_message_id);
      break;
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      td_->create_handler<ReadChannelHistoryQuery>(std::move(promise))->send(channel_id, max_message_id, allow_error);
      break;
    }
    case DialogType::SecretChat: {
      auto secret_chat_id = dialog_id.get_secret_chat_id();
      auto *message = get_message_force(FullMessageId(dialog_id, max_message_id));
      if (message != nullptr) {
        send_closure(G()->secret_chats_manager(), &SecretChatsManager::send_read_history, secret_chat_id, message->date,
                     std::move(promise));
      }
      break;
    }
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

std::pair<int32, vector<MessageId>> MessagesManager::search_dialog_messages(
    DialogId dialog_id, const string &query, UserId sender_user_id, MessageId from_message_id, int32 offset,
    int32 limit, const tl_object_ptr<td_api::SearchMessagesFilter> &filter, int64 &random_id, bool use_db,
    Promise<Unit> &&promise) {
  if (random_id != 0) {
    // request has already been sent before
    auto it = found_dialog_messages_.find(random_id);
    if (it != found_dialog_messages_.end()) {
      auto result = std::move(it->second);
      found_dialog_messages_.erase(it);
      promise.set_value(Unit());
      return result;
    }
    random_id = 0;
  }
  LOG(INFO) << "Search messages with query \"" << query << "\" in " << dialog_id << " sent by " << sender_user_id
            << " filtered by " << to_string(filter) << " from " << from_message_id << " with offset " << offset
            << " and limit " << limit;

  std::pair<int32, vector<MessageId>> result;
  if (limit <= 0) {
    promise.set_error(Status::Error(3, "Parameter limit must be positive"));
    return result;
  }
  if (limit > MAX_SEARCH_MESSAGES) {
    limit = MAX_SEARCH_MESSAGES;
  }
  if (limit <= -offset) {
    promise.set_error(Status::Error(5, "Parameter limit must be greater than -offset"));
    return result;
  }
  if (offset > 0) {
    promise.set_error(Status::Error(5, "Parameter offset must be non-positive"));
    return result;
  }

  if (from_message_id.get() > MessageId::max().get()) {
    from_message_id = MessageId::max();
  }

  if (!from_message_id.is_valid() && from_message_id != MessageId()) {
    promise.set_error(Status::Error(3, "Parameter from_message_id must be identifier of the chat message or 0"));
    return result;
  }
  from_message_id = from_message_id.get_next_server_message_id();

  const Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(6, "Chat not found"));
    return result;
  }

  auto input_user = td_->contacts_manager_->get_input_user(sender_user_id);
  if (sender_user_id.is_valid() && input_user == nullptr) {
    promise.set_error(Status::Error(6, "Wrong sender user identifier specified"));
    return result;
  }

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || found_dialog_messages_.find(random_id) != found_dialog_messages_.end());
  found_dialog_messages_[random_id];  // reserve place for result

  auto filter_type = get_search_messages_filter(filter);
  if (filter_type == SearchMessagesFilter::UnreadMention) {
    if (!query.empty()) {
      promise.set_error(Status::Error(6, "Non-empty query is unsupported with the specified filter"));
      return result;
    }
    if (input_user != nullptr) {
      promise.set_error(Status::Error(6, "Non-empty sender user is unsupported with the specified filter"));
      return result;
    }
  }

  // Trying to use database
  if (query.empty() && G()->parameters().use_message_db && filter_type != SearchMessagesFilter::Empty &&
      input_user == nullptr) {  // TODO support filter by users in the database
    MessageId first_db_message_id = get_first_database_message_id_by_index(d, filter_type);
    int32 message_count = d->message_count_by_index[search_messages_filter_index(filter_type)];
    auto fixed_from_message_id = from_message_id;
    if (fixed_from_message_id == MessageId()) {
      fixed_from_message_id = MessageId::max();
    }
    LOG(INFO) << "Search messages in " << dialog_id << " from " << fixed_from_message_id << ", have up to "
              << first_db_message_id << ", message_count = " << message_count;
    if (use_db &&
        (first_db_message_id.get() < fixed_from_message_id.get() ||
         (first_db_message_id.get() == fixed_from_message_id.get() && offset < 0)) &&
        message_count != -1) {
      LOG(INFO) << "Search messages in database in " << dialog_id << " from " << fixed_from_message_id
                << " and with limit " << limit;
      auto new_promise = PromiseCreator::lambda(
          [random_id, dialog_id, fixed_from_message_id, first_db_message_id, filter_type, offset, limit,
           promise = std::move(promise)](Result<MessagesDbMessagesResult> result) mutable {
            send_closure(G()->messages_manager(), &MessagesManager::on_search_dialog_messages_db_result, random_id,
                         dialog_id, fixed_from_message_id, first_db_message_id, filter_type, offset, limit,
                         std::move(result), std::move(promise));
          });
      MessagesDbMessagesQuery db_query;
      db_query.dialog_id = dialog_id;
      db_query.index_mask = search_messages_filter_index_mask(filter_type);
      db_query.from_message_id = fixed_from_message_id;
      db_query.offset = offset;
      db_query.limit = limit;
      G()->td_db()->get_messages_db_async()->get_messages(db_query, std::move(new_promise));
      return result;
    }
  }

  LOG(DEBUG) << "Search messages on server in " << dialog_id << " with query \"" << query << "\" from user "
             << sender_user_id << " from " << from_message_id << " and with limit " << limit;

  switch (dialog_id.get_type()) {
    case DialogType::None:
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
      td_->create_handler<SearchMessagesQuery>(std::move(promise))
          ->send(dialog_id, query, sender_user_id, std::move(input_user), from_message_id, offset, limit, filter_type,
                 random_id);
      break;
    case DialogType::SecretChat:
      if (filter_type == SearchMessagesFilter::UnreadMention) {
        promise.set_value(Unit());
      } else {
        promise.set_error(Status::Error(500, "Search messages in secret chats is not supported"));
      }
      break;
    default:
      UNREACHABLE();
      promise.set_error(Status::Error(500, "Search messages is not supported"));
  }
  return result;
}

std::pair<int32, vector<FullMessageId>> MessagesManager::search_call_messages(MessageId from_message_id, int32 limit,
                                                                              bool only_missed, int64 &random_id,
                                                                              bool use_db, Promise<Unit> &&promise) {
  if (random_id != 0) {
    // request has already been sent before
    auto it = found_call_messages_.find(random_id);
    if (it != found_call_messages_.end()) {
      auto result = std::move(it->second);
      found_call_messages_.erase(it);
      promise.set_value(Unit());
      return result;
    }
    random_id = 0;
  }
  LOG(INFO) << "Search call messages from " << from_message_id << " with limit " << limit;

  std::pair<int32, vector<FullMessageId>> result;
  if (limit <= 0) {
    promise.set_error(Status::Error(3, "Parameter limit must be positive"));
    return result;
  }
  if (limit > MAX_SEARCH_MESSAGES) {
    limit = MAX_SEARCH_MESSAGES;
  }

  if (from_message_id.get() > MessageId::max().get()) {
    from_message_id = MessageId::max();
  }

  if (!from_message_id.is_valid() && from_message_id != MessageId()) {
    promise.set_error(Status::Error(3, "Parameter from_message_id must be identifier of the chat message or 0"));
    return result;
  }
  from_message_id = from_message_id.get_next_server_message_id();

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || found_call_messages_.find(random_id) != found_call_messages_.end());
  found_call_messages_[random_id];  // reserve place for result

  auto filter_type = only_missed ? SearchMessagesFilter::MissedCall : SearchMessagesFilter::Call;

  // Try to use database
  MessageId first_db_message_id =
      calls_db_state_.first_calls_database_message_id_by_index[search_calls_filter_index(filter_type)];
  int32 message_count = calls_db_state_.message_count_by_index[search_calls_filter_index(filter_type)];
  auto fixed_from_message_id = from_message_id;
  if (fixed_from_message_id == MessageId()) {
    fixed_from_message_id = MessageId::max();
  }
  CHECK(fixed_from_message_id.is_valid() && fixed_from_message_id.is_server());
  LOG(INFO) << "Search call messages from " << fixed_from_message_id << ", have up to " << first_db_message_id
            << ", message_count = " << message_count;
  if (use_db && first_db_message_id.get() < fixed_from_message_id.get() && message_count != -1) {
    LOG(INFO) << "Search messages in database from " << fixed_from_message_id << " and with limit " << limit;

    MessagesDbCallsQuery db_query;
    db_query.index_mask = search_messages_filter_index_mask(filter_type);
    db_query.from_unique_message_id = fixed_from_message_id.get_server_message_id().get();
    db_query.limit = limit;
    G()->td_db()->get_messages_db_async()->get_calls(
        db_query, PromiseCreator::lambda([random_id, first_db_message_id, filter_type, promise = std::move(promise)](
                                             Result<MessagesDbCallsResult> calls_result) mutable {
          send_closure(G()->messages_manager(), &MessagesManager::on_messages_db_calls_result, std::move(calls_result),
                       random_id, first_db_message_id, filter_type, std::move(promise));
        }));
    return result;
  }

  LOG(DEBUG) << "Search call messages on server from " << from_message_id << " and with limit " << limit;
  td_->create_handler<SearchMessagesQuery>(std::move(promise))
      ->send(DialogId(), "", UserId(), nullptr, from_message_id, 0, limit, filter_type, random_id);
  return result;
}

std::pair<int32, vector<MessageId>> MessagesManager::search_dialog_recent_location_messages(DialogId dialog_id,
                                                                                            int32 limit,
                                                                                            int64 &random_id,
                                                                                            Promise<Unit> &&promise) {
  if (random_id != 0) {
    // request has already been sent before
    auto it = found_dialog_recent_location_messages_.find(random_id);
    CHECK(it != found_dialog_recent_location_messages_.end());
    auto result = std::move(it->second);
    found_dialog_recent_location_messages_.erase(it);
    promise.set_value(Unit());
    return result;
  }
  LOG(INFO) << "Search recent location messages in " << dialog_id << " with limit " << limit;

  std::pair<int32, vector<MessageId>> result;
  if (limit <= 0) {
    promise.set_error(Status::Error(3, "Parameter limit must be positive"));
    return result;
  }
  if (limit > MAX_SEARCH_MESSAGES) {
    limit = MAX_SEARCH_MESSAGES;
  }

  const Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(6, "Chat not found"));
    return result;
  }

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 ||
           found_dialog_recent_location_messages_.find(random_id) != found_dialog_recent_location_messages_.end());
  found_dialog_recent_location_messages_[random_id];  // reserve place for result

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
      td_->create_handler<GetRecentLocationsQuery>(std::move(promise))->send(dialog_id, limit, random_id);
      break;
    case DialogType::SecretChat:
      promise.set_value(Unit());
      break;
    default:
      UNREACHABLE();
      promise.set_error(Status::Error(500, "Search messages is not supported"));
  }
  return result;
}

vector<FullMessageId> MessagesManager::get_active_live_location_messages(Promise<Unit> &&promise) {
  if (!G()->parameters().use_message_db) {
    are_active_live_location_messages_loaded_ = true;
  }

  if (!are_active_live_location_messages_loaded_) {
    load_active_live_location_messages_queries_.push_back(std::move(promise));
    if (load_active_live_location_messages_queries_.size() == 1u) {
      LOG(INFO) << "Trying to load active live location messages from database";
      G()->td_db()->get_sqlite_pmc()->get(
          "di_active_live_location_messages", PromiseCreator::lambda([](string value) {
            send_closure(G()->messages_manager(),
                         &MessagesManager::on_load_active_live_location_full_message_ids_from_database,
                         std::move(value));
          }));
    }
    return {};
  }

  promise.set_value(Unit());
  vector<FullMessageId> result;
  for (auto &full_message_id : active_live_location_full_message_ids_) {
    auto m = get_message(full_message_id);
    CHECK(m != nullptr);
    CHECK(m->content->get_id() == MessageLiveLocation::ID);

    auto live_period = static_cast<const MessageLiveLocation *>(m->content.get())->period;
    if (live_period <= G()->unix_time() - m->date) {  // bool is_expired flag?
      // live location is expired
      continue;
    }
    result.push_back(full_message_id);
  }

  return result;
}

void MessagesManager::on_load_active_live_location_full_message_ids_from_database(string value) {
  if (value.empty()) {
    LOG(INFO) << "Active live location messages aren't found in the database";
    on_load_active_live_location_messages_finished();
    return;
  }

  LOG(INFO) << "Successfully loaded active live location messages list of size " << value.size() << " from database";

  auto new_full_message_ids = std::move(active_live_location_full_message_ids_);
  vector<FullMessageId> old_full_message_ids;
  log_event_parse(old_full_message_ids, value).ensure();

  // TODO asynchronously load messages from database
  active_live_location_full_message_ids_.clear();
  for (auto full_message_id : old_full_message_ids) {
    Message *m = get_message_force(full_message_id);
    if (m != nullptr) {
      try_add_active_live_location(full_message_id.get_dialog_id(), m);
    }
  }

  for (auto full_message_id : new_full_message_ids) {
    add_active_live_location(full_message_id);
  }

  on_load_active_live_location_messages_finished();

  if (!new_full_message_ids.empty()) {
    save_active_live_locations();
  }
}

void MessagesManager::on_load_active_live_location_messages_finished() {
  are_active_live_location_messages_loaded_ = true;
  auto promises = std::move(load_active_live_location_messages_queries_);
  load_active_live_location_messages_queries_.clear();
  for (auto &promise : promises) {
    promise.set_value(Unit());
  }
}

void MessagesManager::try_add_active_live_location(DialogId dialog_id, const Message *m) {
  CHECK(m != nullptr);

  if (m->content->get_id() != MessageLiveLocation::ID) {
    return;
  }

  auto live_period = static_cast<const MessageLiveLocation *>(m->content.get())->period;
  if (live_period <= G()->unix_time() - m->date + 1) {  // bool is_expired flag?
    // live location is expired
    return;
  }
  add_active_live_location({dialog_id, m->message_id});
}

void MessagesManager::add_active_live_location(FullMessageId full_message_id) {
  if (!active_live_location_full_message_ids_.insert(full_message_id).second) {
    return;
  }

  // TODO add timer for live location expiration

  if (are_active_live_location_messages_loaded_) {
    save_active_live_locations();
  }
}

bool MessagesManager::delete_active_live_location(DialogId dialog_id, const Message *m) {
  CHECK(m != nullptr);
  return active_live_location_full_message_ids_.erase(FullMessageId{dialog_id, m->message_id}) != 0;
}

void MessagesManager::save_active_live_locations() {
  CHECK(are_active_live_location_messages_loaded_);
  LOG(INFO) << "Save active live locations of size " << active_live_location_full_message_ids_.size() << " to database";
  if (G()->parameters().use_message_db) {
    G()->td_db()->get_sqlite_pmc()->set("di_active_live_location_messages",
                                        log_event_store(active_live_location_full_message_ids_).as_slice().str(),
                                        Auto());
  }
}

MessageId MessagesManager::get_first_database_message_id_by_index(const Dialog *d, SearchMessagesFilter filter) {
  CHECK(d != nullptr);
  auto message_id = filter == SearchMessagesFilter::Empty
                        ? d->first_database_message_id
                        : d->first_database_message_id_by_index[search_messages_filter_index(filter)];
  if (!message_id.is_valid()) {
    if (d->dialog_id.get_type() == DialogType::SecretChat) {
      LOG(ERROR) << "Invalid first_database_message_id_by_index in " << d->dialog_id;
      return MessageId::min();
    }
    return MessageId::max();
  }
  return message_id;
}

void MessagesManager::on_search_dialog_messages_db_result(int64 random_id, DialogId dialog_id,
                                                          MessageId from_message_id, MessageId first_db_message_id,
                                                          SearchMessagesFilter filter_type, int32 offset, int32 limit,
                                                          Result<MessagesDbMessagesResult> result, Promise<> promise) {
  if (result.is_error()) {
    LOG(ERROR) << result.error();
    if (first_db_message_id != MessageId::min() && dialog_id.get_type() != DialogType::SecretChat) {
      found_dialog_messages_.erase(random_id);
    }
    return promise.set_value(Unit());
  }

  auto messages = result.move_as_ok().messages;

  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  auto it = found_dialog_messages_.find(random_id);
  CHECK(it != found_dialog_messages_.end());
  auto &res = it->second.second;

  res.reserve(messages.size());
  for (auto &message : messages) {
    auto m = on_get_message_from_database(dialog_id, d, message);
    if (m != nullptr && first_db_message_id.get() <= m->message_id.get()) {
      res.push_back(m->message_id);
    }
  }

  auto &message_count = d->message_count_by_index[search_messages_filter_index(filter_type)];
  int32 result_size = narrow_cast<int32>(res.size());
  if ((message_count < result_size) ||
      (from_message_id == MessageId::max() && first_db_message_id == MessageId::min() && message_count > result_size &&
       result_size < limit + offset)) {
    LOG(INFO) << "Fix found message count in " << dialog_id << " from " << message_count << " to " << result_size;
    message_count = result_size;
    if (filter_type == SearchMessagesFilter::UnreadMention) {
      d->unread_mention_count = message_count;
      send_update_chat_unread_mention_count(d);
    }
    on_dialog_updated(dialog_id, "on_search_dialog_messages_db_result");
  }
  it->second.first = message_count;
  if (res.empty() && first_db_message_id != MessageId::min() && dialog_id.get_type() != DialogType::SecretChat) {
    LOG(INFO) << "No messages in database found";
    found_dialog_messages_.erase(it);
  } else {
    LOG(INFO) << "Found " << res.size() << " messages out of " << message_count << " in database";
  }
  promise.set_value(Unit());
}

std::pair<int64, vector<FullMessageId>> MessagesManager::offline_search_messages(
    DialogId dialog_id, const string &query, int64 from_search_id, int32 limit,
    const tl_object_ptr<td_api::SearchMessagesFilter> &filter, int64 &random_id, Promise<> &&promise) {
  if (random_id != 0) {
    // request has already been sent before
    auto it = found_fts_messages_.find(random_id);
    CHECK(it != found_fts_messages_.end());
    auto result = std::move(it->second);
    found_fts_messages_.erase(it);
    promise.set_value(Unit());
    return result;
  }

  if (query.empty()) {
    promise.set_value(Unit());
    return Auto();
  }
  if (dialog_id != DialogId() && !have_dialog_force(dialog_id)) {
    promise.set_error(Status::Error(400, "Chat not found"));
    return Auto();
  }
  if (limit <= 0) {
    promise.set_error(Status::Error(400, "Limit must be positive"));
    return Auto();
  }
  if (limit > MAX_SEARCH_MESSAGES) {
    limit = MAX_SEARCH_MESSAGES;
  }

  MessagesDbFtsQuery fts_query;
  fts_query.query = query;
  fts_query.dialog_id = dialog_id;
  fts_query.index_mask = search_messages_filter_index_mask(get_search_messages_filter(filter));
  fts_query.from_search_id = from_search_id;
  fts_query.limit = limit;

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || found_fts_messages_.find(random_id) != found_fts_messages_.end());
  found_fts_messages_[random_id];  // reserve place for result

  G()->td_db()->get_messages_db_async()->get_messages_fts(
      std::move(fts_query),
      PromiseCreator::lambda([random_id, promise = std::move(promise)](Result<MessagesDbFtsResult> fts_result) mutable {
        send_closure(G()->messages_manager(), &MessagesManager::on_messages_db_fts_result, std::move(fts_result),
                     random_id, std::move(promise));
      }));

  return Auto();
}

void MessagesManager::on_messages_db_fts_result(Result<MessagesDbFtsResult> result, int64 random_id,
                                                Promise<> &&promise) {
  if (result.is_error()) {
    found_fts_messages_.erase(random_id);
    return promise.set_error(result.move_as_error());
  }
  auto fts_result = result.move_as_ok();

  auto it = found_fts_messages_.find(random_id);
  CHECK(it != found_fts_messages_.end());
  auto &res = it->second.second;

  res.reserve(fts_result.messages.size());
  for (auto &message : fts_result.messages) {
    auto m = on_get_message_from_database(message.dialog_id, get_dialog_force(message.dialog_id), message.data);
    if (m != nullptr) {
      res.push_back(FullMessageId(message.dialog_id, m->message_id));
    }
  }

  it->second.first = fts_result.next_search_id;

  promise.set_value(Unit());
}

void MessagesManager::on_messages_db_calls_result(Result<MessagesDbCallsResult> result, int64 random_id,
                                                  MessageId first_db_message_id, SearchMessagesFilter filter,
                                                  Promise<> &&promise) {
  if (result.is_error()) {
    found_call_messages_.erase(random_id);
    return promise.set_error(result.move_as_error());
  }
  auto calls_result = result.move_as_ok();

  auto it = found_call_messages_.find(random_id);
  CHECK(it != found_call_messages_.end());
  auto &res = it->second.second;

  res.reserve(calls_result.messages.size());
  for (auto &message : calls_result.messages) {
    auto m = on_get_message_from_database(message.dialog_id, get_dialog_force(message.dialog_id), message.data);

    if (m != nullptr && first_db_message_id.get() <= m->message_id.get()) {
      res.push_back(FullMessageId(message.dialog_id, m->message_id));
    }
  }
  it->second.first = calls_db_state_.message_count_by_index[search_calls_filter_index(filter)];

  if (res.empty() && first_db_message_id != MessageId::min()) {
    LOG(INFO) << "No messages in database found";
    found_call_messages_.erase(it);
  }

  promise.set_value(Unit());
}

std::pair<int32, vector<FullMessageId>> MessagesManager::search_messages(const string &query, int32 offset_date,
                                                                         DialogId offset_dialog_id,
                                                                         MessageId offset_message_id, int32 limit,
                                                                         int64 &random_id, Promise<Unit> &&promise) {
  if (random_id != 0) {
    // request has already been sent before
    auto it = found_messages_.find(random_id);
    CHECK(it != found_messages_.end());
    auto result = std::move(it->second);
    found_messages_.erase(it);
    promise.set_value(Unit());
    return result;
  }

  std::pair<int32, vector<FullMessageId>> result;
  if (limit <= 0) {
    promise.set_error(Status::Error(3, "Parameter limit must be positive"));
    return result;
  }
  if (limit > MAX_SEARCH_MESSAGES) {
    limit = MAX_SEARCH_MESSAGES;
  }

  if (offset_date <= 0) {
    offset_date = std::numeric_limits<int32>::max();
  }
  if (!offset_message_id.is_valid()) {
    offset_message_id = MessageId();
  }
  if (offset_message_id != MessageId() && !offset_message_id.is_server()) {
    promise.set_error(
        Status::Error(3, "Parameter offset_message_id must be identifier of the last found message or 0"));
    return result;
  }

  if (query.empty()) {
    promise.set_value(Unit());
    return result;
  }

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || found_messages_.find(random_id) != found_messages_.end());
  found_messages_[random_id];  // reserve place for result

  LOG(DEBUG) << "Search messages globally with query = \"" << query << "\" from date " << offset_date << ", "
             << offset_dialog_id << ", " << offset_message_id << " and with limit " << limit;

  td_->create_handler<SearchMessagesGlobalQuery>(std::move(promise))
      ->send(query, offset_date, offset_dialog_id, offset_message_id, limit, random_id);
  return result;
}

int64 MessagesManager::get_dialog_message_by_date(DialogId dialog_id, int32 date, Promise<Unit> &&promise) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(5, "Chat not found"));
    return 0;
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    promise.set_error(Status::Error(5, "Can't access the chat"));
    return 0;
  }

  if (date <= 0) {
    date = 1;
  }

  int64 random_id = 0;
  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 ||
           get_dialog_message_by_date_results_.find(random_id) != get_dialog_message_by_date_results_.end());
  get_dialog_message_by_date_results_[random_id];  // reserve place for result

  auto message_id = find_message_by_date(d->messages, date);
  if (message_id.is_valid() && (message_id == d->last_message_id || get_message(d, message_id)->have_next)) {
    get_dialog_message_by_date_results_[random_id] = {dialog_id, message_id};
    promise.set_value(Unit());
    return random_id;
  }

  if (G()->parameters().use_message_db && d->last_database_message_id != MessageId()) {
    CHECK(d->first_database_message_id != MessageId());
    G()->td_db()->get_messages_db_async()->get_dialog_message_by_date(
        dialog_id, d->first_database_message_id, d->last_database_message_id, date,
        PromiseCreator::lambda([actor_id = actor_id(this), dialog_id, date, random_id,
                                promise = std::move(promise)](Result<BufferSlice> result) mutable {
          send_closure(actor_id, &MessagesManager::on_get_dialog_message_by_date_from_database, dialog_id, date,
                       random_id, std::move(result), std::move(promise));
        }));
  } else {
    get_dialog_message_by_date_from_server(d, date, random_id, false, std::move(promise));
  }
  return random_id;
}

MessageId MessagesManager::find_message_by_date(const unique_ptr<Message> &m, int32 date) {
  if (m == nullptr) {
    return MessageId();
  }

  if (m->date > date) {
    return find_message_by_date(m->left, date);
  }

  auto message_id = find_message_by_date(m->right, date);
  if (message_id.is_valid()) {
    return message_id;
  }

  return m->message_id;
}

void MessagesManager::on_get_dialog_message_by_date_from_database(DialogId dialog_id, int32 date, int64 random_id,
                                                                  Result<BufferSlice> result, Promise<Unit> promise) {
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  if (result.is_ok()) {
    Message *m = on_get_message_from_database(dialog_id, d, result.ok());
    if (m != nullptr) {
      auto message_id = find_message_by_date(d->messages, date);
      if (!message_id.is_valid()) {
        LOG(ERROR) << "Failed to find " << m->message_id << " in " << dialog_id << " by date " << date;
        message_id = m->message_id;
      }
      get_dialog_message_by_date_results_[random_id] = {dialog_id, message_id};
      promise.set_value(Unit());
      return;
    }
    // TODO if m == nullptr, we need to just adjust it to the next non-nullptr message, not get from server
  }

  return get_dialog_message_by_date_from_server(d, date, random_id, true, std::move(promise));
}

void MessagesManager::get_dialog_message_by_date_from_server(const Dialog *d, int32 date, int64 random_id,
                                                             bool after_database_search, Promise<Unit> &&promise) {
  CHECK(d != nullptr);
  if (d->have_full_history) {
    // request can be always done locally/in memory. There is no need to send request to the server
    if (after_database_search) {
      return promise.set_value(Unit());
    }

    auto message_id = find_message_by_date(d->messages, date);
    if (message_id.is_valid()) {
      get_dialog_message_by_date_results_[random_id] = {d->dialog_id, message_id};
    }
    promise.set_value(Unit());
    return;
  }
  if (d->dialog_id.get_type() == DialogType::SecretChat) {
    // there is no way to send request to the server
    return promise.set_value(Unit());
  }

  td_->create_handler<GetDialogMessageByDateQuery>(std::move(promise))->send(d->dialog_id, date, random_id);
}

void MessagesManager::on_get_dialog_message_by_date_success(DialogId dialog_id, int32 date, int64 random_id,
                                                            vector<tl_object_ptr<telegram_api::Message>> &&messages) {
  auto it = get_dialog_message_by_date_results_.find(random_id);
  CHECK(it != get_dialog_message_by_date_results_.end());
  auto &result = it->second;
  CHECK(result == FullMessageId());

  for (auto &message : messages) {
    auto message_date = get_message_date(message);
    auto message_dialog_id = get_message_dialog_id(message);
    if (message_dialog_id != dialog_id) {
      LOG(ERROR) << "Receive message in wrong " << message_dialog_id << " instead of " << dialog_id;
      continue;
    }
    if (message_date != 0 && message_date <= date) {
      result = on_get_message(std::move(message), false, dialog_id.get_type() == DialogType::Channel, false, false,
                              "on_get_dialog_message_by_date_success");
      if (result != FullMessageId()) {
        const Dialog *d = get_dialog(dialog_id);
        CHECK(d != nullptr);
        auto message_id = find_message_by_date(d->messages, date);
        if (!message_id.is_valid()) {
          LOG(ERROR) << "Failed to find " << result.get_message_id() << " in " << dialog_id << " by date " << date;
          message_id = result.get_message_id();
        }
        get_dialog_message_by_date_results_[random_id] = {dialog_id, message_id};
        // TODO result must be adjusted by local messages
        return;
      }
    }
  }
}

void MessagesManager::on_get_dialog_message_by_date_fail(int64 random_id) {
  auto erased = get_dialog_message_by_date_results_.erase(random_id);
  CHECK(erased > 0);
}

tl_object_ptr<td_api::message> MessagesManager::get_dialog_message_by_date_object(int64 random_id) {
  auto it = get_dialog_message_by_date_results_.find(random_id);
  CHECK(it != get_dialog_message_by_date_results_.end());
  auto full_message_id = std::move(it->second);
  get_dialog_message_by_date_results_.erase(it);
  return get_message_object(full_message_id);
}

void MessagesManager::preload_newer_messages(const Dialog *d, MessageId max_message_id) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  MessagesConstIterator p(d, max_message_id);
  int32 limit = MAX_GET_HISTORY * 3 / 10;
  while (*p != nullptr && limit-- > 0) {
    ++p;
    if (*p) {
      max_message_id = (*p)->message_id;
    }
  }
  if (limit > 0 && (d->last_message_id == MessageId() || max_message_id.get() < d->last_message_id.get())) {
    // need to preload some new messages
    LOG(INFO) << "Preloading newer after " << max_message_id;
    load_messages(d->dialog_id, max_message_id, -MAX_GET_HISTORY + 1, MAX_GET_HISTORY, 3, false, Promise<Unit>());
  }
}

void MessagesManager::preload_older_messages(const Dialog *d, MessageId min_message_id) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  /*
    if (d->first_remote_message_id == -1) {
      // nothing left to preload from server
      return;
    }
  */
  MessagesConstIterator p(d, min_message_id);
  int32 limit = MAX_GET_HISTORY * 3 / 10 + 1;
  while (*p != nullptr && limit-- > 0) {
    min_message_id = (*p)->message_id;
    --p;
  }
  if (limit > 0) {
    // need to preload some old messages
    LOG(INFO) << "Preloading older before " << min_message_id;
    load_messages(d->dialog_id, min_message_id, 0, MAX_GET_HISTORY / 2, 3, false, Promise<Unit>());
  }
}

void MessagesManager::on_get_history_from_database(DialogId dialog_id, MessageId from_message_id, int32 offset,
                                                   int32 limit, bool from_the_end, bool only_local,
                                                   vector<BufferSlice> &&messages, Promise<Unit> &&promise) {
  CHECK(-limit < offset && offset <= 0);
  CHECK(offset < 0 || from_the_end);

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    LOG(WARNING) << "Ignore result of get_history_from_database in " << dialog_id;
    promise.set_value(Unit());
    return;
  }

  auto d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  LOG(INFO) << "Receive " << messages.size() << " history messages from database "
            << (from_the_end ? "from the end " : "") << "in " << dialog_id << " from " << from_message_id
            << " with offset " << offset << " and limit " << limit << ". First database message is "
            << d->first_database_message_id << ", have_full_history = " << d->have_full_history;

  if (messages.empty() && from_the_end && d->have_full_history) {
    set_dialog_is_empty(d, "on_get_history_from_database empty");
  }

  bool have_next = false;
  bool need_update = false;
  bool need_update_dialog_pos = false;
  bool added_new_message = false;
  MessageId last_added_message_id;
  Message *next_message = nullptr;
  Dependencies dependencies;
  bool is_first = true;
  for (auto &message_slice : messages) {
    if (!d->first_database_message_id.is_valid() && !d->have_full_history) {
      break;
    }
    auto message = make_unique<Message>();
    log_event_parse(*message, message_slice.as_slice()).ensure();
    if (message->message_id.get() < d->first_database_message_id.get()) {
      if (d->have_full_history) {
        LOG(ERROR) << "Have full history in the " << dialog_id << " and receive " << message->message_id
                   << " from database, but first database message is " << d->first_database_message_id;
      } else {
        break;
      }
    }
    if (!have_next &&
        (from_the_end || (is_first && offset < -1 && message->message_id.get() <= from_message_id.get())) &&
        message->message_id.get() < d->last_message_id.get()) {
      // last message in the dialog must be attached to the next local message
      have_next = true;
    }

    message->have_previous = false;
    message->have_next = have_next;
    message->from_database = true;

    auto old_message = get_message(d, message->message_id);
    if (old_message == nullptr && message->content->get_id() == MessageText::ID) {
      auto web_page_id = static_cast<const MessageText *>(message->content.get())->web_page_id;
      if (web_page_id.is_valid()) {
        td_->web_pages_manager_->have_web_page_force(web_page_id);
      }
    }
    Message *m = old_message ? old_message
                             : add_message_to_dialog(d, std::move(message), false, &need_update,
                                                     &need_update_dialog_pos, "on_get_history_from_database");
    if (m != nullptr) {
      if (!have_next) {
        last_added_message_id = m->message_id;
      }
      if (old_message == nullptr) {
        add_message_dependencies(dependencies, dialog_id, m);
        added_new_message = true;
      } else if (m->message_id != from_message_id) {
        added_new_message = true;
      }
      if (next_message != nullptr && !next_message->have_previous) {
        LOG(INFO) << "Fix have_previous for " << next_message->message_id;
        next_message->have_previous = true;
        attach_message_to_previous(d, next_message->message_id);
      }

      have_next = true;
      next_message = m;
    }
    is_first = false;
  }
  resolve_dependencies_force(dependencies);

  if (!added_new_message && !only_local && dialog_id.get_type() != DialogType::SecretChat) {
    if (from_the_end) {
      from_message_id = MessageId();
    }
    load_messages(dialog_id, from_message_id, offset, limit, 1, false, std::move(promise));
    return;
  }

  if (from_the_end && last_added_message_id.is_valid()) {
    // CHECK(d->first_database_message_id.is_valid());
    // CHECK(last_added_message_id.get() >= d->first_database_message_id.get());
    if (last_added_message_id.get() > d->last_message_id.get() && d->last_new_message_id.is_valid()) {
      set_dialog_last_message_id(d, last_added_message_id, "on_get_history_from_database");
      need_update_dialog_pos = true;
    }
    if (last_added_message_id.get() != d->last_database_message_id.get()) {
      set_dialog_last_database_message_id(d, last_added_message_id, "on_get_history_from_database");
      if (last_added_message_id.get() < d->first_database_message_id.get() ||
          !d->first_database_message_id.is_valid()) {
        CHECK(next_message != nullptr);
        CHECK(d->have_full_history);
        LOG(ERROR) << "Fix first database message id in " << dialog_id << " from " << d->first_database_message_id
                   << " to " << next_message->message_id;
        set_dialog_first_database_message_id(d, next_message->message_id, "on_get_history_from_database");
      }
      on_dialog_updated(dialog_id, "on_get_history_from_database");
    }
  }

  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "on_get_history_from_database");
  }

  promise.set_value(Unit());
}

void MessagesManager::get_history_from_the_end(DialogId dialog_id, bool from_database, bool only_local,
                                               Promise<Unit> &&promise) {
  CHECK(dialog_id.is_valid());
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // can't get history in dialogs without read access
    return promise.set_value(Unit());
  }
  const int32 limit = MAX_GET_HISTORY;
  if (from_database && G()->parameters().use_message_db) {
    LOG(INFO) << "Get history from the end of " << dialog_id << " from database";
    MessagesDbMessagesQuery db_query;
    db_query.dialog_id = dialog_id;
    db_query.from_message_id = MessageId::max();
    db_query.limit = limit;
    G()->td_db()->get_messages_db_async()->get_messages(
        db_query, PromiseCreator::lambda([dialog_id, only_local, limit, actor_id = actor_id(this),
                                          promise = std::move(promise)](MessagesDbMessagesResult result) mutable {
          send_closure(actor_id, &MessagesManager::on_get_history_from_database, dialog_id, MessageId::max(), 0, limit,
                       true, only_local, std::move(result.messages), std::move(promise));
        }));
  } else {
    if (only_local || dialog_id.get_type() == DialogType::SecretChat) {
      promise.set_value(Unit());
      return;
    }

    LOG(INFO) << "Get history from the end of " << dialog_id << " from server";
    td_->create_handler<GetHistoryQuery>(std::move(promise))->send_get_from_the_end(dialog_id, limit);
  }
}

void MessagesManager::get_history(DialogId dialog_id, MessageId from_message_id, int32 offset, int32 limit,
                                  bool from_database, bool only_local, Promise<Unit> &&promise) {
  CHECK(dialog_id.is_valid());
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // can't get history in dialogs without read access
    return promise.set_value(Unit());
  }
  if (from_database && G()->parameters().use_message_db) {
    LOG(INFO) << "Get history in " << dialog_id << " from " << from_message_id << " with offset " << offset
              << " and limit " << limit << " from database";
    MessagesDbMessagesQuery db_query;
    db_query.dialog_id = dialog_id;
    db_query.from_message_id = from_message_id;
    db_query.offset = offset;
    db_query.limit = limit;
    G()->td_db()->get_messages_db_async()->get_messages(
        db_query,
        PromiseCreator::lambda([dialog_id, from_message_id, offset, limit, only_local, actor_id = actor_id(this),
                                promise = std::move(promise)](MessagesDbMessagesResult result) mutable {
          send_closure(actor_id, &MessagesManager::on_get_history_from_database, dialog_id, from_message_id, offset,
                       limit, false, only_local, std::move(result.messages), std::move(promise));
        }));
  } else {
    if (only_local || dialog_id.get_type() == DialogType::SecretChat) {
      return promise.set_value(Unit());
    }

    LOG(INFO) << "Get history in " << dialog_id << " from " << from_message_id << " with offset " << offset
              << " and limit " << limit << " from server";
    td_->create_handler<GetHistoryQuery>(std::move(promise))
        ->send(dialog_id, from_message_id.get_next_server_message_id(), offset, limit);
  }
}

void MessagesManager::load_messages(DialogId dialog_id, MessageId from_message_id, int32 offset, int32 limit,
                                    int left_tries, bool only_local, Promise<Unit> &&promise) {
  LOG(INFO) << "Load " << (only_local ? "local " : "") << "messages in " << dialog_id << " from " << from_message_id
            << " with offset = " << offset << " and limit = " << limit << ". " << left_tries << " tries left";
  CHECK(offset <= 0);
  CHECK(left_tries > 0);
  only_local |= dialog_id.get_type() == DialogType::SecretChat;
  if (!only_local) {
    Dialog *d = get_dialog(dialog_id);
    if (d != nullptr && d->have_full_history) {
      LOG(INFO) << "Have full history in " << dialog_id << ", so don't need to get chat history from server";
      only_local = true;
    }
  }
  bool from_database = (left_tries > 2 || only_local) && G()->parameters().use_message_db;
  // TODO do not send requests to database if (from_message_id < d->first_database_message_id ||
  // !d->first_database_message_id.is_valid()) && !d->have_full_history

  if (from_message_id == MessageId()) {
    get_history_from_the_end(dialog_id, from_database, only_local, std::move(promise));
    return;
  }
  if (offset >= -1) {
    // get history before some server or local message
    limit = min(max(limit + offset + 1, MAX_GET_HISTORY / 2), MAX_GET_HISTORY);
    offset = -1;
  } else {
    // get history around some server or local message
    int32 messages_to_load = max(MAX_GET_HISTORY, limit);
    int32 max_add = messages_to_load - limit;
    offset -= max_add;
    limit = MAX_GET_HISTORY;
  }
  get_history(dialog_id, from_message_id, offset, limit, from_database, only_local, std::move(promise));
}

tl_object_ptr<td_api::message> MessagesManager::get_message_object(FullMessageId full_message_id) {
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog(dialog_id);
  return get_message_object(dialog_id, get_message_force(d, full_message_id.get_message_id()));
}

tl_object_ptr<td_api::message> MessagesManager::get_message_object(DialogId dialog_id, const Message *message) const {
  if (message == nullptr) {
    return nullptr;
  }

  // TODO get_message_sending_state_object
  tl_object_ptr<td_api::MessageSendingState> sending_state;
  if (message->is_failed_to_send) {
    sending_state = make_tl_object<td_api::messageSendingStateFailed>();
  } else if (message->message_id.is_yet_unsent()) {
    sending_state = make_tl_object<td_api::messageSendingStatePending>();
  }

  bool can_delete = true;
  auto dialog_type = dialog_id.get_type();
  if (dialog_type == DialogType::Channel) {
    auto dialog_status = td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id());
    can_delete = can_delete_channel_message(dialog_status, message, td_->auth_manager_->is_bot());
  }

  DialogId my_dialog_id(td_->contacts_manager_->get_my_id("get_message_object"));
  bool can_delete_for_self = false;
  bool can_delete_for_all_users = can_delete && can_revoke_message(dialog_id, message);
  if (can_delete) {
    switch (dialog_type) {
      case DialogType::User:
      case DialogType::Chat:
        // TODO allow to delete yet unsent message just for self
        can_delete_for_self = !message->message_id.is_yet_unsent() || dialog_id == my_dialog_id;
        break;
      case DialogType::Channel:
      case DialogType::SecretChat:
        can_delete_for_self = !can_delete_for_all_users;
        break;
      case DialogType::None:
      default:
        UNREACHABLE();
    }
  }

  bool is_outgoing = message->is_outgoing || sending_state != nullptr;
  if (dialog_id == my_dialog_id) {
    // in Saved Messages all messages without forward_info->from_dialog_id should be outgoing
    if (message->forward_info == nullptr || !message->forward_info->from_dialog_id.is_valid()) {
      is_outgoing = true;
    }
  }
  return make_tl_object<td_api::message>(
      message->message_id.get(), td_->contacts_manager_->get_user_id_object(message->sender_user_id, "sender_user_id"),
      dialog_id.get(), std::move(sending_state), is_outgoing, can_edit_message(dialog_id, message, false, true),
      can_forward_message(dialog_id, message), can_delete_for_self, can_delete_for_all_users, message->is_channel_post,
      message->contains_unread_mention, message->date, message->edit_date,
      get_message_forward_info_object(message->forward_info), message->reply_to_message_id.get(), message->ttl,
      message->ttl_expires_at != 0 ? max(message->ttl_expires_at - Time::now(), 1e-3) : message->ttl,
      td_->contacts_manager_->get_user_id_object(message->via_bot_user_id, "via_bot_user_id"),
      message->author_signature, message->views, message->media_album_id,
      get_message_content_object(message->content.get(), message->date, message->is_content_secret),
      get_reply_markup_object(message->reply_markup));
}

tl_object_ptr<td_api::messages> MessagesManager::get_messages_object(int32 total_count, DialogId dialog_id,
                                                                     const vector<MessageId> &message_ids) {
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  return get_messages_object(total_count, transform(message_ids, [this, dialog_id, d](MessageId message_id) {
                               return get_message_object(dialog_id, get_message_force(d, message_id));
                             }));
}

tl_object_ptr<td_api::messages> MessagesManager::get_messages_object(int32 total_count,
                                                                     const vector<FullMessageId> &full_message_ids) {
  return get_messages_object(total_count, transform(full_message_ids, [this](FullMessageId full_message_id) {
                               return get_message_object(full_message_id);
                             }));
}

tl_object_ptr<td_api::messages> MessagesManager::get_messages_object(
    int32 total_count, vector<tl_object_ptr<td_api::message>> &&messages) {
  if (total_count == -1) {
    total_count = narrow_cast<int32>(messages.size());
  }
  return td_api::make_object<td_api::messages>(total_count, std::move(messages));
}

bool MessagesManager::need_skip_bot_commands(DialogId dialog_id, bool is_bot) const {
  if (is_bot) {
    return false;
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return !td_->contacts_manager_->is_user_bot(dialog_id.get_user_id());
    case DialogType::SecretChat: {
      auto user_id = td_->contacts_manager_->get_secret_chat_user_id(dialog_id.get_secret_chat_id());
      return !td_->contacts_manager_->is_user_bot(user_id);
    }
    case DialogType::Chat:
    case DialogType::Channel:
    case DialogType::None:
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

Result<FormattedText> MessagesManager::process_input_caption(DialogId dialog_id,
                                                             tl_object_ptr<td_api::formattedText> &&text,
                                                             bool is_bot) const {
  if (text == nullptr) {
    return FormattedText();
  }
  TRY_RESULT(entities, get_message_entities(td_->contacts_manager_.get(), std::move(text->entities_)));
  TRY_STATUS(fix_formatted_text(text->text_, entities, true, false, need_skip_bot_commands(dialog_id, is_bot), false));
  return FormattedText{std::move(text->text_), std::move(entities)};
}

Result<InputMessageText> MessagesManager::process_input_message_text(
    DialogId dialog_id, tl_object_ptr<td_api::InputMessageContent> &&input_message_content, bool is_bot,
    bool for_draft) const {
  CHECK(input_message_content != nullptr);
  CHECK(input_message_content->get_id() == td_api::inputMessageText::ID);
  auto input_message_text = static_cast<td_api::inputMessageText *>(input_message_content.get());
  if (input_message_text->text_ == nullptr) {
    if (for_draft) {
      return InputMessageText{FormattedText(), input_message_text->disable_web_page_preview_,
                              input_message_text->clear_draft_};
    }

    return Status::Error(400, "Message text can't be empty");
  }

  TRY_RESULT(entities,
             get_message_entities(td_->contacts_manager_.get(), std::move(input_message_text->text_->entities_)));
  TRY_STATUS(fix_formatted_text(input_message_text->text_->text_, entities, for_draft, false,
                                need_skip_bot_commands(dialog_id, is_bot), for_draft));
  return InputMessageText{FormattedText{std::move(input_message_text->text_->text_), std::move(entities)},
                          input_message_text->disable_web_page_preview_, input_message_text->clear_draft_};
}

Result<std::pair<Location, int32>> MessagesManager::process_input_message_location(
    tl_object_ptr<td_api::InputMessageContent> &&input_message_content) {
  CHECK(input_message_content != nullptr);
  CHECK(input_message_content->get_id() == td_api::inputMessageLocation::ID);
  auto input_location = static_cast<const td_api::inputMessageLocation *>(input_message_content.get());

  Location location(input_location->location_);
  if (location.empty()) {
    return Status::Error(400, "Wrong location specified");
  }

  auto period = input_location->live_period_;
  if (period != 0 && (period < MIN_LIVE_LOCATION_PERIOD || period > MAX_LIVE_LOCATION_PERIOD)) {
    return Status::Error(400, "Wrong live location period specified");
  }

  return std::make_pair(std::move(location), period);
}

Result<Venue> MessagesManager::process_input_message_venue(
    tl_object_ptr<td_api::InputMessageContent> &&input_message_content) {
  CHECK(input_message_content != nullptr);
  CHECK(input_message_content->get_id() == td_api::inputMessageVenue::ID);
  auto venue = std::move(static_cast<td_api::inputMessageVenue *>(input_message_content.get())->venue_);

  if (venue == nullptr) {
    return Status::Error(400, "Venue can't be empty");
  }

  if (!clean_input_string(venue->title_)) {
    return Status::Error(400, "Venue title must be encoded in UTF-8");
  }
  if (!clean_input_string(venue->address_)) {
    return Status::Error(400, "Venue address must be encoded in UTF-8");
  }
  if (!clean_input_string(venue->provider_)) {
    return Status::Error(400, "Venue provider must be encoded in UTF-8");
  }
  if (!clean_input_string(venue->id_)) {
    return Status::Error(400, "Venue identifier must be encoded in UTF-8");
  }

  Venue result(venue);
  if (result.empty()) {
    return Status::Error(400, "Wrong venue location specified");
  }

  return result;
}

Result<Contact> MessagesManager::process_input_message_contact(
    tl_object_ptr<td_api::InputMessageContent> &&input_message_content) {
  CHECK(input_message_content != nullptr);
  CHECK(input_message_content->get_id() == td_api::inputMessageContact::ID);
  auto contact = std::move(static_cast<td_api::inputMessageContact *>(input_message_content.get())->contact_);

  if (!clean_input_string(contact->phone_number_)) {
    return Status::Error(400, "Phone number must be encoded in UTF-8");
  }
  if (!clean_input_string(contact->first_name_)) {
    return Status::Error(400, "First name must be encoded in UTF-8");
  }
  if (!clean_input_string(contact->last_name_)) {
    return Status::Error(400, "Last name must be encoded in UTF-8");
  }

  return Contact(contact->phone_number_, contact->first_name_, contact->last_name_, contact->user_id_);
}

Result<Game> MessagesManager::process_input_message_game(
    tl_object_ptr<td_api::InputMessageContent> &&input_message_content) const {
  CHECK(input_message_content != nullptr);
  CHECK(input_message_content->get_id() == td_api::inputMessageGame::ID);
  auto input_message_game = move_tl_object_as<td_api::inputMessageGame>(input_message_content);

  UserId bot_user_id(input_message_game->bot_user_id_);
  if (!td_->contacts_manager_->have_input_user(bot_user_id)) {
    return Status::Error(400, "Game owner bot is not accessible");
  }

  if (!clean_input_string(input_message_game->game_short_name_)) {
    return Status::Error(400, "Game short name must be encoded in UTF-8");
  }

  // TODO validate game_short_name
  if (input_message_game->game_short_name_.empty()) {
    return Status::Error(400, "Game short name must be non-empty");
  }

  return Game(bot_user_id, std::move(input_message_game->game_short_name_));
}

SecretInputMedia MessagesManager::get_secret_input_media(const MessageContent *content,
                                                         tl_object_ptr<telegram_api::InputEncryptedFile> input_file,
                                                         BufferSlice thumbnail, int32 layer) {
  switch (content->get_id()) {
    case MessageAnimation::ID: {
      auto m = static_cast<const MessageAnimation *>(content);
      return td_->animations_manager_->get_secret_input_media(m->file_id, std::move(input_file), m->caption.text,
                                                              std::move(thumbnail));
    }
    case MessageAudio::ID: {
      auto m = static_cast<const MessageAudio *>(content);
      return td_->audios_manager_->get_secret_input_media(m->file_id, std::move(input_file), m->caption.text,
                                                          std::move(thumbnail));
    }
    case MessageContact::ID: {
      auto m = static_cast<const MessageContact *>(content);
      return m->contact.get_secret_input_media_contact();
    }
    case MessageDocument::ID: {
      auto m = static_cast<const MessageDocument *>(content);
      return td_->documents_manager_->get_secret_input_media(m->file_id, std::move(input_file), m->caption.text,
                                                             std::move(thumbnail));
    }
    case MessageLocation::ID: {
      auto m = static_cast<const MessageLocation *>(content);
      return m->location.get_secret_input_media_geo_point();
    }
    case MessagePhoto::ID: {
      auto m = static_cast<const MessagePhoto *>(content);
      return photo_get_secret_input_media(td_->file_manager_.get(), m->photo, std::move(input_file), m->caption.text,
                                          std::move(thumbnail));
    }
    case MessageSticker::ID: {
      auto m = static_cast<const MessageSticker *>(content);
      return td_->stickers_manager_->get_secret_input_media(m->file_id, std::move(input_file), std::move(thumbnail));
    }
    case MessageText::ID: {
      CHECK(input_file == nullptr);
      CHECK(thumbnail.empty());
      auto m = static_cast<const MessageText *>(content);
      return td_->web_pages_manager_->get_secret_input_media(m->web_page_id);
    }
    case MessageVenue::ID: {
      auto m = static_cast<const MessageVenue *>(content);
      return m->venue.get_secret_input_media_venue();
    }
    case MessageVideo::ID: {
      auto m = static_cast<const MessageVideo *>(content);
      return td_->videos_manager_->get_secret_input_media(m->file_id, std::move(input_file), m->caption.text,
                                                          std::move(thumbnail));
    }
    case MessageVideoNote::ID: {
      auto m = static_cast<const MessageVideoNote *>(content);
      return td_->video_notes_manager_->get_secret_input_media(m->file_id, std::move(input_file), std::move(thumbnail),
                                                               layer);
    }
    case MessageVoiceNote::ID: {
      auto m = static_cast<const MessageVoiceNote *>(content);
      return td_->voice_notes_manager_->get_secret_input_media(m->file_id, std::move(input_file), m->caption.text);
    }
    case MessageLiveLocation::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageUnsupported::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      break;
    default:
      UNREACHABLE();
  }
  return SecretInputMedia{};
}

tl_object_ptr<telegram_api::invoice> MessagesManager::get_input_invoice(const Invoice &invoice) const {
  int32 flags = 0;
  if (invoice.is_test) {
    flags |= telegram_api::invoice::TEST_MASK;
  }
  if (invoice.need_name) {
    flags |= telegram_api::invoice::NAME_REQUESTED_MASK;
  }
  if (invoice.need_phone_number) {
    flags |= telegram_api::invoice::PHONE_REQUESTED_MASK;
  }
  if (invoice.need_email_address) {
    flags |= telegram_api::invoice::EMAIL_REQUESTED_MASK;
  }
  if (invoice.need_shipping_address) {
    flags |= telegram_api::invoice::SHIPPING_ADDRESS_REQUESTED_MASK;
  }
  if (invoice.send_phone_number_to_provider) {
    flags |= telegram_api::invoice::PHONE_TO_PROVIDER_MASK;
  }
  if (invoice.send_email_address_to_provider) {
    flags |= telegram_api::invoice::EMAIL_TO_PROVIDER_MASK;
  }
  if (invoice.is_flexible) {
    flags |= telegram_api::invoice::FLEXIBLE_MASK;
  }

  vector<tl_object_ptr<telegram_api::labeledPrice>> prices;
  prices.reserve(invoice.price_parts.size());
  for (auto &price : invoice.price_parts) {
    prices.push_back(make_tl_object<telegram_api::labeledPrice>(price.label, price.amount));
  }

  return make_tl_object<telegram_api::invoice>(
      flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
      false /*ignored*/, false /*ignored*/, false /*ignored*/, invoice.currency, std::move(prices));
}

tl_object_ptr<telegram_api::inputWebDocument> MessagesManager::get_input_web_document(const Photo &photo) const {
  if (photo.id == -2) {
    return nullptr;
  }

  CHECK(photo.photos.size() == 1);
  const PhotoSize &size = photo.photos[0];
  CHECK(size.file_id.is_valid());

  vector<tl_object_ptr<telegram_api::DocumentAttribute>> attributes;
  if (size.dimensions.width != 0 && size.dimensions.height != 0) {
    attributes.push_back(
        make_tl_object<telegram_api::documentAttributeImageSize>(size.dimensions.width, size.dimensions.height));
  }

  auto file_view = td_->file_manager_->get_file_view(size.file_id);
  CHECK(file_view.has_url());

  auto file_name = get_url_file_name(file_view.url());
  return make_tl_object<telegram_api::inputWebDocument>(
      file_view.url(), size.size, MimeType::from_extension(PathView(file_name).extension(), "image/jpeg"),
      std::move(attributes));
}

tl_object_ptr<telegram_api::inputMediaInvoice> MessagesManager::get_input_media_invoice(
    const MessageInvoice *message_invoice) const {
  CHECK(message_invoice != nullptr);
  int32 flags = 0;
  auto input_web_document = get_input_web_document(message_invoice->photo);
  if (input_web_document != nullptr) {
    flags |= telegram_api::inputMediaInvoice::PHOTO_MASK;
  }

  return make_tl_object<telegram_api::inputMediaInvoice>(
      flags, message_invoice->title, message_invoice->description, std::move(input_web_document),
      get_input_invoice(message_invoice->invoice), BufferSlice(message_invoice->payload),
      message_invoice->provider_token,
      telegram_api::make_object<telegram_api::dataJSON>(
          message_invoice->provider_data.empty() ? "null" : message_invoice->provider_data),
      message_invoice->start_parameter);
}

tl_object_ptr<telegram_api::InputMedia> MessagesManager::get_input_media(
    const MessageContent *content, tl_object_ptr<telegram_api::InputFile> input_file,
    tl_object_ptr<telegram_api::InputFile> input_thumbnail, int32 ttl) {
  switch (content->get_id()) {
    case MessageAnimation::ID: {
      auto m = static_cast<const MessageAnimation *>(content);
      return td_->animations_manager_->get_input_media(m->file_id, std::move(input_file), std::move(input_thumbnail));
    }
    case MessageAudio::ID: {
      auto m = static_cast<const MessageAudio *>(content);
      return td_->audios_manager_->get_input_media(m->file_id, std::move(input_file), std::move(input_thumbnail));
    }
    case MessageContact::ID: {
      auto m = static_cast<const MessageContact *>(content);
      return m->contact.get_input_media_contact();
    }
    case MessageDocument::ID: {
      auto m = static_cast<const MessageDocument *>(content);
      return td_->documents_manager_->get_input_media(m->file_id, std::move(input_file), std::move(input_thumbnail));
    }
    case MessageGame::ID: {
      auto m = static_cast<const MessageGame *>(content);
      return m->game.get_input_media_game(td_);
    }
    case MessageInvoice::ID: {
      auto m = static_cast<const MessageInvoice *>(content);
      return get_input_media_invoice(m);
    }
    case MessageLiveLocation::ID: {
      auto m = static_cast<const MessageLiveLocation *>(content);
      return make_tl_object<telegram_api::inputMediaGeoLive>(m->location.get_input_geo_point(), m->period);
    }
    case MessageLocation::ID: {
      auto m = static_cast<const MessageLocation *>(content);
      return m->location.get_input_media_geo_point();
    }
    case MessagePhoto::ID: {
      auto m = static_cast<const MessagePhoto *>(content);
      return photo_get_input_media(td_->file_manager_.get(), m->photo, std::move(input_file), ttl);
    }
    case MessageSticker::ID: {
      auto m = static_cast<const MessageSticker *>(content);
      return td_->stickers_manager_->get_input_media(m->file_id, std::move(input_file), std::move(input_thumbnail));
    }
    case MessageVenue::ID: {
      auto m = static_cast<const MessageVenue *>(content);
      return m->venue.get_input_media_venue();
    }
    case MessageVideo::ID: {
      auto m = static_cast<const MessageVideo *>(content);
      return td_->videos_manager_->get_input_media(m->file_id, std::move(input_file), std::move(input_thumbnail), ttl);
    }
    case MessageVideoNote::ID: {
      auto m = static_cast<const MessageVideoNote *>(content);
      return td_->video_notes_manager_->get_input_media(m->file_id, std::move(input_file), std::move(input_thumbnail));
    }
    case MessageVoiceNote::ID: {
      auto m = static_cast<const MessageVoiceNote *>(content);
      return td_->voice_notes_manager_->get_input_media(m->file_id, std::move(input_file));
    }
    case MessageText::ID:
    case MessageUnsupported::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      break;
    default:
      UNREACHABLE();
  }
  return nullptr;
}

void MessagesManager::delete_message_content_thumbnail(MessageContent *content) {
  switch (content->get_id()) {
    case MessageAnimation::ID: {
      auto m = static_cast<MessageAnimation *>(content);
      return td_->animations_manager_->delete_animation_thumbnail(m->file_id);
    }
    case MessageAudio::ID: {
      auto m = static_cast<MessageAudio *>(content);
      return td_->audios_manager_->delete_audio_thumbnail(m->file_id);
    }
    case MessageDocument::ID: {
      auto m = static_cast<MessageDocument *>(content);
      return td_->documents_manager_->delete_document_thumbnail(m->file_id);
    }
    case MessagePhoto::ID: {
      auto m = static_cast<MessagePhoto *>(content);
      return photo_delete_thumbnail(m->photo);
    }
    case MessageSticker::ID: {
      auto m = static_cast<MessageSticker *>(content);
      return td_->stickers_manager_->delete_sticker_thumbnail(m->file_id);
    }
    case MessageVideo::ID: {
      auto m = static_cast<MessageVideo *>(content);
      return td_->videos_manager_->delete_video_thumbnail(m->file_id);
    }
    case MessageVideoNote::ID: {
      auto m = static_cast<MessageVideoNote *>(content);
      return td_->video_notes_manager_->delete_video_note_thumbnail(m->file_id);
    }
    case MessageContact::ID:
    case MessageGame::ID:
    case MessageInvoice::ID:
    case MessageLiveLocation::ID:
    case MessageLocation::ID:
    case MessageVenue::ID:
    case MessageVoiceNote::ID:
    case MessageText::ID:
    case MessageUnsupported::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      break;
    default:
      UNREACHABLE();
  }
}

MessagesManager::Message *MessagesManager::get_message_to_send(Dialog *d, MessageId reply_to_message_id,
                                                               bool disable_notification, bool from_background,
                                                               unique_ptr<MessageContent> &&content,
                                                               bool *need_update_dialog_pos,
                                                               unique_ptr<MessageForwardInfo> forward_info) {
  CHECK(d != nullptr);
  MessageId message_id = get_next_yet_unsent_message_id(d);
  DialogId dialog_id = d->dialog_id;
  LOG(INFO) << "Create " << message_id << " in " << dialog_id;

  auto dialog_type = dialog_id.get_type();
  auto my_id = td_->contacts_manager_->get_my_id("get_message_to_send");

  auto m = make_unique<Message>();
  m->random_y = get_random_y(message_id);
  m->message_id = message_id;
  bool is_channel_post = is_broadcast_channel(dialog_id);
  if (is_channel_post) {
    // sender of the post can be hidden
    if (td_->contacts_manager_->get_channel_sign_messages(dialog_id.get_channel_id())) {
      m->author_signature = td_->contacts_manager_->get_user_title(my_id);
    }
  } else {
    m->sender_user_id = my_id;
  }
  m->date = G()->unix_time();
  m->reply_to_message_id = reply_to_message_id;
  m->is_channel_post = is_channel_post;
  m->is_outgoing = dialog_id != DialogId(my_id);
  m->from_background = from_background;
  m->views = is_channel_post ? 1 : 0;
  m->content = std::move(content);
  m->forward_info = std::move(forward_info);

  if (td_->auth_manager_->is_bot()) {
    m->disable_notification = disable_notification;
  } else {
    auto notification_settings = get_notification_settings(NotificationSettingsScope(dialog_id.get()), true);
    CHECK(notification_settings != nullptr);
    m->disable_notification = notification_settings->silent_send_message;
  }

  if (dialog_type == DialogType::SecretChat) {
    m->ttl = td_->contacts_manager_->get_secret_chat_ttl(dialog_id.get_secret_chat_id());
    if (is_service_message_content(m->content->get_id())) {
      m->ttl = 0;
    }
    m->is_content_secret = is_secret_message_content(m->ttl, m->content->get_id());
    if (reply_to_message_id.is_valid()) {
      auto *reply_to_message = get_message_force(d, reply_to_message_id);
      if (reply_to_message != nullptr) {
        m->reply_to_random_id = reply_to_message->random_id;
      } else {
        m->reply_to_message_id = MessageId();
      }
    }
  }

  m->have_previous = true;
  m->have_next = true;

  do {
    m->random_id = Random::secure_int64();
  } while (m->random_id == 0 || message_random_ids_.find(m->random_id) != message_random_ids_.end());
  message_random_ids_.insert(m->random_id);

  bool need_update = false;
  CHECK(have_input_peer(dialog_id, AccessRights::Read));
  auto result = add_message_to_dialog(d, std::move(m), false, &need_update, need_update_dialog_pos, "send message");
  CHECK(result != nullptr);
  return result;
}

int64 MessagesManager::begin_send_message(DialogId dialog_id, const Message *m) {
  LOG(INFO) << "Begin to send " << FullMessageId(dialog_id, m->message_id) << " with random_id = " << m->random_id;
  CHECK(m->random_id != 0 && being_sent_messages_.find(m->random_id) == being_sent_messages_.end());
  being_sent_messages_[m->random_id] = FullMessageId(dialog_id, m->message_id);
  debug_being_sent_messages_[m->random_id] = dialog_id;
  return m->random_id;
}

Status MessagesManager::can_send_message(DialogId dialog_id) const {
  if (!have_input_peer(dialog_id, AccessRights::Write)) {
    return Status::Error(400, "Have no write access to the chat");
  }

  if (dialog_id.get_type() == DialogType::Channel) {
    auto channel_id = dialog_id.get_channel_id();
    auto channel_type = td_->contacts_manager_->get_channel_type(channel_id);
    auto channel_status = td_->contacts_manager_->get_channel_status(channel_id);

    switch (channel_type) {
      case ChannelType::Unknown:
      case ChannelType::Megagroup:
        if (!channel_status.can_send_messages()) {
          return Status::Error(400, "Have no rights to send a message");
        }
        break;
      case ChannelType::Broadcast: {
        if (!channel_status.can_post_messages()) {
          return Status::Error(400, "Need administrator rights in the channel chat");
        }
        break;
      }
      default:
        UNREACHABLE();
    }
  }
  return Status::OK();
}

Status MessagesManager::can_send_message_content(DialogId dialog_id, const MessageContent *content,
                                                 bool is_forward) const {
  auto dialog_type = dialog_id.get_type();
  int32 secret_chat_layer = std::numeric_limits<int32>::max();
  if (dialog_type == DialogType::SecretChat) {
    auto secret_chat_id = dialog_id.get_secret_chat_id();
    secret_chat_layer = td_->contacts_manager_->get_secret_chat_layer(secret_chat_id);
  }

  bool can_send_messages = true;
  bool can_send_media = true;
  bool can_send_stickers = true;
  bool can_send_animations = true;
  bool can_send_games = true;

  switch (dialog_type) {
    case DialogType::User:
    case DialogType::Chat:
      // ok
      break;
    case DialogType::Channel: {
      auto channel_status = td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id());
      can_send_messages = channel_status.can_send_messages();
      can_send_media = channel_status.can_send_media();
      can_send_stickers = channel_status.can_send_stickers();
      can_send_animations = channel_status.can_send_animations();
      can_send_games = channel_status.can_send_games();
      break;
    }
    case DialogType::SecretChat:
      can_send_games = false;
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  switch (content->get_id()) {
    case MessageAnimation::ID:
      if (!can_send_animations) {
        return Status::Error(400, "Not enough rights to send animations to the chat");
      }
      break;
    case MessageAudio::ID:
      if (!can_send_media) {
        return Status::Error(400, "Not enough rights to send audios to the chat");
      }
      break;
    case MessageContact::ID:
      if (!can_send_messages) {
        return Status::Error(400, "Not enough rights to send contacts to the chat");
      }
      break;
    case MessageDocument::ID:
      if (!can_send_media) {
        return Status::Error(400, "Not enough rights to send documents to the chat");
      }
      break;
    case MessageGame::ID:
      switch (dialog_id.get_type()) {
        case DialogType::User:
        case DialogType::Chat:
          // ok
          break;
        case DialogType::Channel: {
          auto channel_type = td_->contacts_manager_->get_channel_type(dialog_id.get_channel_id());
          if (channel_type == ChannelType::Broadcast) {
            return Status::Error(400, "Games can't be sent to channel chats");
          }
          break;
        }
        case DialogType::SecretChat:
          return Status::Error(400, "Games can't be sent to secret chats");
        case DialogType::None:
        default:
          UNREACHABLE();
      }

      if (!can_send_games) {
        return Status::Error(400, "Not enough rights to send games to the chat");
      }
      break;
    case MessageInvoice::ID:
      if (!is_forward) {
        switch (dialog_type) {
          case DialogType::User:
            // ok
            break;
          case DialogType::Chat:
          case DialogType::Channel:
          case DialogType::SecretChat:
            return Status::Error(400, "Invoices can be sent only to private chats");
          case DialogType::None:
          default:
            UNREACHABLE();
        }
      }
      break;
    case MessageLiveLocation::ID:
      if (!can_send_messages) {
        return Status::Error(400, "Not enough rights to send live locations to the chat");
      }
      break;
    case MessageLocation::ID:
      if (!can_send_messages) {
        return Status::Error(400, "Not enough rights to send locations to the chat");
      }
      break;
    case MessagePhoto::ID:
      if (!can_send_media) {
        return Status::Error(400, "Not enough rights to send photos to the chat");
      }
      break;
    case MessageSticker::ID:
      if (!can_send_stickers) {
        return Status::Error(400, "Not enough rights to send stickers to the chat");
      }
      break;
    case MessageText::ID:
      if (!can_send_messages) {
        return Status::Error(400, "Not enough rights to send text messages to the chat");
      }
      break;
    case MessageVenue::ID:
      if (!can_send_messages) {
        return Status::Error(400, "Not enough rights to send venues to the chat");
      }
      break;
    case MessageVideo::ID:
      if (!can_send_media) {
        return Status::Error(400, "Not enough rights to send videos to the chat");
      }
      break;
    case MessageVideoNote::ID:
      if (!can_send_media) {
        return Status::Error(400, "Not enough rights to send video notes to the chat");
      }
      if (secret_chat_layer < SecretChatActor::VOICE_NOTES_LAYER) {
        return Status::Error(400, PSLICE()
                                      << "Video notes can't be sent to secret chat with layer " << secret_chat_layer);
      }
      break;
    case MessageVoiceNote::ID:
      if (!can_send_media) {
        return Status::Error(400, "Not enough rights to send voice notes to the chat");
      }
      break;
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageUnsupported::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      UNREACHABLE();
  }
  return Status::OK();
}

MessageId MessagesManager::get_persistent_message_id(const Dialog *d, MessageId message_id) const {
  if (!message_id.is_valid()) {
    return MessageId();
  }
  if (message_id.is_yet_unsent()) {
    // it is possible that user tries to do something with an already sent message by its temporary id
    // we need to use real message in this case and transparently replace message_id
    auto it = d->yet_unsent_message_id_to_persistent_message_id.find(message_id);
    if (it != d->yet_unsent_message_id_to_persistent_message_id.end()) {
      return it->second;
    }
  }

  return message_id;
}

MessageId MessagesManager::get_reply_to_message_id(Dialog *d, MessageId message_id) {
  CHECK(d != nullptr);
  message_id = get_persistent_message_id(d, message_id);
  const Message *reply_to_message = get_message_force(d, message_id);
  if (reply_to_message == nullptr || message_id.is_yet_unsent() ||
      (message_id.is_local() && d->dialog_id.get_type() != DialogType::SecretChat)) {
    // TODO local replies to local messages can be allowed
    // TODO replies to yet unsent messages can be allowed with special handling of them on application restart
    return MessageId();
  }
  return message_id;
}

FormattedText MessagesManager::get_message_content_caption(const MessageContent *content) {
  switch (content->get_id()) {
    case MessageAnimation::ID:
      return static_cast<const MessageAnimation *>(content)->caption;
    case MessageAudio::ID:
      return static_cast<const MessageAudio *>(content)->caption;
    case MessageDocument::ID:
      return static_cast<const MessageDocument *>(content)->caption;
    case MessagePhoto::ID:
      return static_cast<const MessagePhoto *>(content)->caption;
    case MessageVideo::ID:
      return static_cast<const MessageVideo *>(content)->caption;
    case MessageVoiceNote::ID:
      return static_cast<const MessageVoiceNote *>(content)->caption;
    default:
      return FormattedText();
  }
}

int32 MessagesManager::get_message_content_duration(const MessageContent *content) const {
  CHECK(content != nullptr);
  switch (content->get_id()) {
    case MessageAnimation::ID: {
      auto animation_file_id = static_cast<const MessageAnimation *>(content)->file_id;
      return td_->animations_manager_->get_animation_duration(animation_file_id);
    }
    case MessageAudio::ID: {
      auto audio_file_id = static_cast<const MessageAudio *>(content)->file_id;
      return td_->audios_manager_->get_audio_duration(audio_file_id);
    }
    case MessageVideo::ID: {
      auto video_file_id = static_cast<const MessageVideo *>(content)->file_id;
      return td_->videos_manager_->get_video_duration(video_file_id);
    }
    case MessageVideoNote::ID: {
      auto video_note_file_id = static_cast<const MessageVideoNote *>(content)->file_id;
      return td_->video_notes_manager_->get_video_note_duration(video_note_file_id);
    }
    case MessageVoiceNote::ID: {
      auto voice_file_id = static_cast<const MessageVoiceNote *>(content)->file_id;
      return td_->voice_notes_manager_->get_voice_note_duration(voice_file_id);
    }
  }
  return 0;
}

FileId MessagesManager::get_message_content_file_id(const MessageContent *content) {
  switch (content->get_id()) {
    case MessageAnimation::ID:
      return static_cast<const MessageAnimation *>(content)->file_id;
    case MessageAudio::ID:
      return static_cast<const MessageAudio *>(content)->file_id;
    case MessageDocument::ID:
      return static_cast<const MessageDocument *>(content)->file_id;
    case MessagePhoto::ID:
      for (auto &size : static_cast<const MessagePhoto *>(content)->photo.photos) {
        if (size.type == 'i') {
          return size.file_id;
        }
      }
      break;
    case MessageSticker::ID:
      return static_cast<const MessageSticker *>(content)->file_id;
    case MessageVideo::ID:
      return static_cast<const MessageVideo *>(content)->file_id;
    case MessageVideoNote::ID:
      return static_cast<const MessageVideoNote *>(content)->file_id;
    case MessageVoiceNote::ID:
      return static_cast<const MessageVoiceNote *>(content)->file_id;
    default:
      break;
  }
  return FileId();
}

void MessagesManager::update_message_content_file_id_remote(MessageContent *content, FileId file_id) {
  if (file_id.get_remote() == 0) {
    return;
  }
  FileId *old_file_id = [&]() {
    switch (content->get_id()) {
      case MessageAnimation::ID:
        return &static_cast<MessageAnimation *>(content)->file_id;
      case MessageAudio::ID:
        return &static_cast<MessageAudio *>(content)->file_id;
      case MessageDocument::ID:
        return &static_cast<MessageDocument *>(content)->file_id;
      case MessageSticker::ID:
        return &static_cast<MessageSticker *>(content)->file_id;
      case MessageVideo::ID:
        return &static_cast<MessageVideo *>(content)->file_id;
      case MessageVideoNote::ID:
        return &static_cast<MessageVideoNote *>(content)->file_id;
      case MessageVoiceNote::ID:
        return &static_cast<MessageVoiceNote *>(content)->file_id;
      default:
        return static_cast<FileId *>(nullptr);
    }
  }();
  if (old_file_id != nullptr && *old_file_id == file_id && old_file_id->get_remote() == 0) {
    *old_file_id = file_id;
  }
}

FileId MessagesManager::get_message_content_thumbnail_file_id(const MessageContent *content) const {
  switch (content->get_id()) {
    case MessageAnimation::ID:
      return td_->animations_manager_->get_animation_thumbnail_file_id(
          static_cast<const MessageAnimation *>(content)->file_id);
    case MessageAudio::ID:
      return td_->audios_manager_->get_audio_thumbnail_file_id(static_cast<const MessageAudio *>(content)->file_id);
    case MessageDocument::ID:
      return td_->documents_manager_->get_document_thumbnail_file_id(
          static_cast<const MessageDocument *>(content)->file_id);
    case MessagePhoto::ID:
      for (auto &size : static_cast<const MessagePhoto *>(content)->photo.photos) {
        if (size.type == 't') {
          return size.file_id;
        }
      }
      break;
    case MessageSticker::ID:
      return td_->stickers_manager_->get_sticker_thumbnail_file_id(
          static_cast<const MessageSticker *>(content)->file_id);
    case MessageVideo::ID:
      return td_->videos_manager_->get_video_thumbnail_file_id(static_cast<const MessageVideo *>(content)->file_id);
    case MessageVideoNote::ID:
      return td_->video_notes_manager_->get_video_note_thumbnail_file_id(
          static_cast<const MessageVideoNote *>(content)->file_id);
    case MessageVoiceNote::ID:
      return FileId();
    default:
      break;
  }
  return FileId();
}

vector<FileId> MessagesManager::get_message_file_ids(const Message *message) const {
  auto content = message->content.get();
  switch (content->get_id()) {
    case MessagePhoto::ID:
      return transform(static_cast<const MessagePhoto *>(content)->photo.photos,
                       [](auto &size) { return size.file_id; });
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageDocument::ID:
    case MessageSticker::ID:
    case MessageVideo::ID:
    case MessageVideoNote::ID:
    case MessageVoiceNote::ID: {
      vector<FileId> result;
      result.reserve(2);
      FileId file_id = get_message_content_file_id(content);
      if (file_id.is_valid()) {
        result.push_back(file_id);
      }
      FileId thumbnail_file_id = get_message_content_thumbnail_file_id(content);
      if (file_id.is_valid()) {
        result.push_back(thumbnail_file_id);
      }
      return result;
    }
    default:
      return {};
  }
}

void MessagesManager::cancel_send_message_query(DialogId dialog_id, unique_ptr<Message> &m) {
  CHECK(m != nullptr);
  CHECK(m->content != nullptr);
  CHECK(m->message_id.is_yet_unsent());
  LOG(INFO) << "Cancel send message query for " << m->message_id;

  auto file_id = get_message_content_file_id(m->content.get());
  if (file_id.is_valid()) {
    auto it = being_uploaded_files_.find(file_id);
    if (it != being_uploaded_files_.end()) {
      LOG(INFO) << "Cancel upload file " << file_id << " for " << m->message_id;
      td_->file_manager_->upload(file_id, nullptr, 0, 0);
      being_uploaded_files_.erase(it);
    }
  }

  auto thumbnail_file_id = get_message_content_thumbnail_file_id(m->content.get());
  if (thumbnail_file_id.is_valid()) {
    auto it = being_uploaded_thumbnails_.find(thumbnail_file_id);
    if (it != being_uploaded_thumbnails_.end()) {
      LOG(INFO) << "Cancel upload thumbnail file " << thumbnail_file_id << " for " << m->message_id;
      td_->file_manager_->upload(thumbnail_file_id, nullptr, 0, 0);
      being_uploaded_thumbnails_.erase(it);
    }
  }

  if (!m->send_query_ref.empty()) {
    LOG(INFO) << "Cancel send query for " << m->message_id;
    cancel_query(m->send_query_ref);
    m->send_query_ref = NetQueryRef();
  }

  if (m->send_message_logevent_id != 0) {
    LOG(INFO) << "Delete send message log event for " << m->message_id;
    BinlogHelper::erase(G()->td_db()->get_binlog(), m->send_message_logevent_id);
    m->send_message_logevent_id = 0;
  }

  if (m->reply_to_message_id.is_valid() && !m->reply_to_message_id.is_yet_unsent()) {
    auto it = replied_by_yet_unsent_messages_.find({dialog_id, m->reply_to_message_id});
    CHECK(it != replied_by_yet_unsent_messages_.end());
    it->second--;
    CHECK(it->second >= 0);
    if (it->second == 0) {
      replied_by_yet_unsent_messages_.erase(it);
    }
  }

  if (m->media_album_id != 0) {
    send_closure_later(actor_id(this), &MessagesManager::on_upload_message_media_finished, m->media_album_id, dialog_id,
                       m->message_id, Status::OK());
  }

  if (G()->parameters().use_file_db) {  // ResourceManager::Mode::Baseline
    auto queue_id = get_sequence_dispatcher_id(dialog_id, m->content->get_id());
    if (queue_id & 1) {
      auto queue_it = yet_unsent_media_queues_.find(queue_id);
      if (queue_it != yet_unsent_media_queues_.end()) {
        auto &queue = queue_it->second;
        LOG(INFO) << "Delete " << m->message_id << " from queue " << queue_id;
        queue.erase(m->message_id.get());
        if (queue.empty()) {
          yet_unsent_media_queues_.erase(queue_it);
        } else {
          on_yet_unsent_media_queue_updated(dialog_id);
        }
      }
    }
  }
}

bool MessagesManager::is_message_auto_read(DialogId dialog_id, bool is_outgoing, bool only_content) const {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      auto user_id = dialog_id.get_user_id();
      if (user_id == td_->contacts_manager_->get_my_id("is_message_auto_read")) {
        return true;
      }
      if (is_outgoing && td_->contacts_manager_->is_user_bot(user_id)) {
        return true;
      }
      return false;
    }
    case DialogType::Chat:
      // TODO auto_read message content and messages sent to group with bots only
      return false;
    case DialogType::Channel: {
      if (only_content) {
        return false;
      }
      auto channel_type = td_->contacts_manager_->get_channel_type(dialog_id.get_channel_id());
      return channel_type != ChannelType::Megagroup;
    }
    case DialogType::SecretChat:
      return false;
    case DialogType::None:
      return false;
    default:
      UNREACHABLE();
      return false;
  }
}

void MessagesManager::add_message_dependencies(Dependencies &dependencies, DialogId dialog_id, const Message *m) {
  dependencies.user_ids.insert(m->sender_user_id);
  dependencies.user_ids.insert(m->via_bot_user_id);
  if (m->forward_info != nullptr) {
    dependencies.user_ids.insert(m->forward_info->sender_user_id);
    if (m->forward_info->dialog_id.is_valid() && dependencies.dialog_ids.insert(m->forward_info->dialog_id).second) {
      add_dialog_dependencies(dependencies, m->forward_info->dialog_id);
    }
    if (m->forward_info->from_dialog_id.is_valid() &&
        dependencies.dialog_ids.insert(m->forward_info->from_dialog_id).second) {
      add_dialog_dependencies(dependencies, m->forward_info->from_dialog_id);
    }
  }
  switch (m->content->get_id()) {
    case MessageText::ID: {
      auto content = static_cast<const MessageText *>(m->content.get());
      for (auto &entity : content->text.entities) {
        if (entity.user_id.is_valid()) {
          dependencies.user_ids.insert(entity.user_id);
        }
      }
      dependencies.web_page_ids.insert(content->web_page_id);
      break;
    }
    case MessageAnimation::ID:
      break;
    case MessageAudio::ID:
      break;
    case MessageContact::ID: {
      auto content = static_cast<const MessageContact *>(m->content.get());
      dependencies.user_ids.insert(content->contact.get_user_id());
      break;
    }
    case MessageDocument::ID:
      break;
    case MessageGame::ID: {
      auto content = static_cast<const MessageGame *>(m->content.get());
      dependencies.user_ids.insert(content->game.get_bot_user_id());
      break;
    }
    case MessageInvoice::ID:
      break;
    case MessageLiveLocation::ID:
      break;
    case MessageLocation::ID:
      break;
    case MessagePhoto::ID:
      break;
    case MessageSticker::ID:
      break;
    case MessageVenue::ID:
      break;
    case MessageVideo::ID:
      break;
    case MessageVideoNote::ID:
      break;
    case MessageVoiceNote::ID:
      break;
    case MessageChatCreate::ID: {
      auto content = static_cast<const MessageChatCreate *>(m->content.get());
      dependencies.user_ids.insert(content->participant_user_ids.begin(), content->participant_user_ids.end());
      break;
    }
    case MessageChatChangeTitle::ID:
      break;
    case MessageChatChangePhoto::ID:
      break;
    case MessageChatDeletePhoto::ID:
      break;
    case MessageChatDeleteHistory::ID:
      break;
    case MessageChatAddUsers::ID: {
      auto content = static_cast<const MessageChatAddUsers *>(m->content.get());
      dependencies.user_ids.insert(content->user_ids.begin(), content->user_ids.end());
      break;
    }
    case MessageChatJoinedByLink::ID:
      break;
    case MessageChatDeleteUser::ID: {
      auto content = static_cast<const MessageChatDeleteUser *>(m->content.get());
      dependencies.user_ids.insert(content->user_id);
      break;
    }
    case MessageChatMigrateTo::ID: {
      auto content = static_cast<const MessageChatMigrateTo *>(m->content.get());
      dependencies.channel_ids.insert(content->migrated_to_channel_id);
      break;
    }
    case MessageChannelCreate::ID:
      break;
    case MessageChannelMigrateFrom::ID: {
      auto content = static_cast<const MessageChannelMigrateFrom *>(m->content.get());
      dependencies.chat_ids.insert(content->migrated_from_chat_id);
      break;
    }
    case MessagePinMessage::ID:
      break;
    case MessageGameScore::ID:
      break;
    case MessageScreenshotTaken::ID:
      break;
    case MessageChatSetTtl::ID:
      break;
    case MessageUnsupported::ID:
      break;
    case MessageCall::ID:
      break;
    case MessagePaymentSuccessful::ID:
      break;
    case MessageContactRegistered::ID:
      break;
    case MessageExpiredPhoto::ID:
      break;
    case MessageExpiredVideo::ID:
      break;
    case MessageCustomServiceAction::ID:
      break;
    case MessageWebsiteConnected::ID:
      break;
    default:
      UNREACHABLE();
      break;
  }
}

void MessagesManager::add_dialog_dependencies(Dependencies &dependencies, DialogId dialog_id) {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      dependencies.user_ids.insert(dialog_id.get_user_id());
      break;
    case DialogType::Chat:
      dependencies.chat_ids.insert(dialog_id.get_chat_id());
      break;
    case DialogType::Channel:
      dependencies.channel_ids.insert(dialog_id.get_channel_id());
      break;
    case DialogType::SecretChat:
      dependencies.secret_chat_ids.insert(dialog_id.get_secret_chat_id());
      break;
    case DialogType::None:
      break;
    default:
      UNREACHABLE();
  }
}

void MessagesManager::resolve_dependencies_force(const Dependencies &dependencies) {
  for (auto user_id : dependencies.user_ids) {
    if (user_id.is_valid() && !td_->contacts_manager_->have_user_force(user_id)) {
      LOG(ERROR) << "Can't find " << user_id;
    }
  }
  for (auto chat_id : dependencies.chat_ids) {
    if (chat_id.is_valid() && !td_->contacts_manager_->have_chat_force(chat_id)) {
      LOG(ERROR) << "Can't find " << chat_id;
    }
  }
  for (auto channel_id : dependencies.channel_ids) {
    if (channel_id.is_valid() && !td_->contacts_manager_->have_channel_force(channel_id)) {
      LOG(ERROR) << "Can't find " << channel_id;
    }
  }
  for (auto secret_chat_id : dependencies.secret_chat_ids) {
    if (secret_chat_id.is_valid() && !td_->contacts_manager_->have_secret_chat_force(secret_chat_id)) {
      LOG(ERROR) << "Can't find " << secret_chat_id;
    }
  }
  for (auto dialog_id : dependencies.dialog_ids) {
    if (dialog_id.is_valid() && !have_dialog_force(dialog_id)) {
      LOG(ERROR) << "Can't find " << dialog_id;
      force_create_dialog(dialog_id, "resolve_dependencies_force");
    }
  }
  for (auto web_page_id : dependencies.web_page_ids) {
    if (web_page_id.is_valid()) {
      td_->web_pages_manager_->have_web_page_force(web_page_id);
    }
  }
}

class MessagesManager::SendMessageLogEvent {
 public:
  DialogId dialog_id;
  const Message *m_in;
  unique_ptr<Message> m_out;

  SendMessageLogEvent() : dialog_id(), m_in(nullptr) {
  }

  SendMessageLogEvent(DialogId dialog_id, const Message *m) : dialog_id(dialog_id), m_in(m) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id, storer);
    td::store(*m_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id, parser);
    CHECK(m_out == nullptr);
    m_out = make_unique<Message>();
    td::parse(*m_out, parser);
  }
};

Result<MessageId> MessagesManager::send_message(DialogId dialog_id, MessageId reply_to_message_id,
                                                bool disable_notification, bool from_background,
                                                tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                tl_object_ptr<td_api::InputMessageContent> &&input_message_content) {
  if (input_message_content == nullptr) {
    return Status::Error(5, "Can't send message without content");
  }

  LOG(INFO) << "Begin to send message to " << dialog_id << " in reply to " << reply_to_message_id;
  if (input_message_content->get_id() == td_api::inputMessageForwarded::ID) {
    auto input_message = static_cast<const td_api::inputMessageForwarded *>(input_message_content.get());
    return forward_message(dialog_id, DialogId(input_message->from_chat_id_), MessageId(input_message->message_id_),
                           disable_notification, from_background, input_message->in_game_share_);
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(5, "Chat not found");
  }

  TRY_STATUS(can_send_message(dialog_id));
  TRY_RESULT(message_reply_markup, get_dialog_reply_markup(dialog_id, std::move(reply_markup)));
  TRY_RESULT(message_content, process_input_message_content(dialog_id, std::move(input_message_content)));

  // there must be no errors after get_message_to_send call

  bool need_update_dialog_pos = false;
  Message *m = get_message_to_send(
      d, get_reply_to_message_id(d, reply_to_message_id), disable_notification, from_background,
      dup_message_content(dialog_id, message_content.content.get(), false), &need_update_dialog_pos);
  m->reply_markup = std::move(message_reply_markup);
  m->via_bot_user_id = message_content.via_bot_user_id;
  m->disable_web_page_preview = message_content.disable_web_page_preview;
  m->clear_draft = message_content.clear_draft;
  if (message_content.ttl > 0) {
    m->ttl = message_content.ttl;
    m->is_content_secret = is_secret_message_content(m->ttl, m->content->get_id());
  }

  send_update_new_message(d, m, true);
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "send_message");
  }

  if (message_content.clear_draft) {
    update_dialog_draft_message(d, nullptr, false, true);
  }

  auto message_id = m->message_id;
  save_send_message_logevent(dialog_id, m);
  do_send_message(dialog_id, m);
  return message_id;
}

Result<MessagesManager::InputMessageContent> MessagesManager::process_input_message_content(
    DialogId dialog_id, tl_object_ptr<td_api::InputMessageContent> &&input_message_content) const {
  if (input_message_content == nullptr) {
    return Status::Error(5, "Can't send message without content");
  }

  bool is_secret = dialog_id.get_type() == DialogType::SecretChat;
  int32 message_type = input_message_content->get_id();

  bool have_file = true;
  // TODO: send from secret chat to common
  Result<FileId> r_file_id;
  tl_object_ptr<td_api::inputThumbnail> input_thumbnail;
  vector<FileId> sticker_file_ids;
  switch (message_type) {
    case td_api::inputMessageAnimation::ID: {
      auto input_message = static_cast<td_api::inputMessageAnimation *>(input_message_content.get());
      r_file_id = td_->file_manager_->get_input_file_id(FileType::Animation, input_message->animation_, dialog_id,
                                                        false, is_secret, true);
      input_thumbnail = std::move(input_message->thumbnail_);
      break;
    }
    case td_api::inputMessageAudio::ID: {
      auto input_message = static_cast<td_api::inputMessageAudio *>(input_message_content.get());
      r_file_id =
          td_->file_manager_->get_input_file_id(FileType::Audio, input_message->audio_, dialog_id, false, is_secret);
      input_thumbnail = std::move(input_message->album_cover_thumbnail_);
      break;
    }
    case td_api::inputMessageDocument::ID: {
      auto input_message = static_cast<td_api::inputMessageDocument *>(input_message_content.get());
      r_file_id = td_->file_manager_->get_input_file_id(FileType::Document, input_message->document_, dialog_id, false,
                                                        is_secret, true);
      input_thumbnail = std::move(input_message->thumbnail_);
      break;
    }
    case td_api::inputMessagePhoto::ID: {
      auto input_message = static_cast<td_api::inputMessagePhoto *>(input_message_content.get());
      r_file_id =
          td_->file_manager_->get_input_file_id(FileType::Photo, input_message->photo_, dialog_id, false, is_secret);
      input_thumbnail = std::move(input_message->thumbnail_);
      if (!input_message->added_sticker_file_ids_.empty()) {
        sticker_file_ids =
            td_->stickers_manager_->get_attached_sticker_file_ids(input_message->added_sticker_file_ids_);
      }
      break;
    }
    case td_api::inputMessageSticker::ID: {
      auto input_message = static_cast<td_api::inputMessageSticker *>(input_message_content.get());
      r_file_id = td_->file_manager_->get_input_file_id(FileType::Sticker, input_message->sticker_, dialog_id, false,
                                                        is_secret);
      input_thumbnail = std::move(input_message->thumbnail_);
      break;
    }
    case td_api::inputMessageVideo::ID: {
      auto input_message = static_cast<td_api::inputMessageVideo *>(input_message_content.get());
      r_file_id =
          td_->file_manager_->get_input_file_id(FileType::Video, input_message->video_, dialog_id, false, is_secret);
      input_thumbnail = std::move(input_message->thumbnail_);
      if (!input_message->added_sticker_file_ids_.empty()) {
        sticker_file_ids =
            td_->stickers_manager_->get_attached_sticker_file_ids(input_message->added_sticker_file_ids_);
      }
      break;
    }
    case td_api::inputMessageVideoNote::ID: {
      auto input_message = static_cast<td_api::inputMessageVideoNote *>(input_message_content.get());
      r_file_id = td_->file_manager_->get_input_file_id(FileType::VideoNote, input_message->video_note_, dialog_id,
                                                        false, is_secret);
      input_thumbnail = std::move(input_message->thumbnail_);
      break;
    }
    case td_api::inputMessageVoiceNote::ID: {
      auto input_message = static_cast<td_api::inputMessageVoiceNote *>(input_message_content.get());
      r_file_id = td_->file_manager_->get_input_file_id(FileType::VoiceNote, input_message->voice_note_, dialog_id,
                                                        false, is_secret);
      break;
    }
    default:
      have_file = false;
      break;
  }
  // TODO is path of files must be stored in bytes instead of UTF-8 string?

  FileId file_id;
  FileView file_view;
  string file_name;
  string mime_type;
  if (have_file) {
    if (r_file_id.is_error()) {
      return Status::Error(7, r_file_id.error().message());
    }
    file_id = r_file_id.ok();
    CHECK(file_id.is_valid());
    file_view = td_->file_manager_->get_file_view(file_id);
    auto suggested_name = file_view.suggested_name();
    const PathView path_view(suggested_name);
    file_name = path_view.file_name().str();
    mime_type = MimeType::from_extension(path_view.extension());
  }

  FileId thumbnail_file_id;
  PhotoSize thumbnail;
  if (input_thumbnail != nullptr) {
    auto r_thumbnail_file_id =
        td_->file_manager_->get_input_thumbnail_file_id(input_thumbnail->thumbnail_, dialog_id, is_secret);
    if (r_thumbnail_file_id.is_error()) {
      LOG(WARNING) << "Ignore thumbnail file: " << r_thumbnail_file_id.error().message();
    } else {
      thumbnail_file_id = r_thumbnail_file_id.ok();
      CHECK(thumbnail_file_id.is_valid());

      thumbnail.type = 't';
      thumbnail.dimensions = get_dimensions(input_thumbnail->width_, input_thumbnail->height_);
      thumbnail.file_id = thumbnail_file_id;

      FileView thumbnail_file_view = td_->file_manager_->get_file_view(thumbnail_file_id);
      if (thumbnail_file_view.has_remote_location()) {
        // TODO td->file_manager_->delete_remote_location(thumbnail_file_id);
      }
    }
  }

  LOG(INFO) << "Send file " << file_id << " and thumbnail " << thumbnail_file_id;

  bool disable_web_page_preview = false;
  bool clear_draft = false;
  unique_ptr<MessageContent> content;
  UserId via_bot_user_id;
  int32 ttl = 0;
  bool is_bot = td_->auth_manager_->is_bot();
  switch (message_type) {
    case td_api::inputMessageText::ID: {
      TRY_RESULT(input_message_text, process_input_message_text(dialog_id, std::move(input_message_content), is_bot));
      disable_web_page_preview = input_message_text.disable_web_page_preview;
      clear_draft = input_message_text.clear_draft;

      WebPageId web_page_id;
      if (!disable_web_page_preview &&
          (dialog_id.get_type() != DialogType::Channel ||
           td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id()).can_add_web_page_previews())) {
        web_page_id = td_->web_pages_manager_->get_web_page_by_url(
            get_first_url(input_message_text.text.text, input_message_text.text.entities));
      }
      content = make_unique<MessageText>(std::move(input_message_text.text), web_page_id);
      break;
    }
    case td_api::inputMessageAnimation::ID: {
      auto input_animation = static_cast<td_api::inputMessageAnimation *>(input_message_content.get());

      TRY_RESULT(caption, process_input_caption(dialog_id, std::move(input_animation->caption_), is_bot));

      td_->animations_manager_->create_animation(
          file_id, thumbnail, std::move(file_name), std::move(mime_type), input_animation->duration_,
          get_dimensions(input_animation->width_, input_animation->height_), false);

      content = make_unique<MessageAnimation>(file_id, std::move(caption));
      break;
    }
    case td_api::inputMessageAudio::ID: {
      auto input_audio = static_cast<td_api::inputMessageAudio *>(input_message_content.get());

      if (!clean_input_string(input_audio->title_)) {
        return Status::Error(400, "Audio title must be encoded in UTF-8");
      }
      if (!clean_input_string(input_audio->performer_)) {
        return Status::Error(400, "Audio performer must be encoded in UTF-8");
      }
      TRY_RESULT(caption, process_input_caption(dialog_id, std::move(input_audio->caption_), is_bot));

      td_->audios_manager_->create_audio(file_id, thumbnail, std::move(file_name), std::move(mime_type),
                                         input_audio->duration_, std::move(input_audio->title_),
                                         std::move(input_audio->performer_), false);

      content = make_unique<MessageAudio>(file_id, std::move(caption));
      break;
    }
    case td_api::inputMessageDocument::ID: {
      auto input_document = static_cast<td_api::inputMessageDocument *>(input_message_content.get());

      TRY_RESULT(caption, process_input_caption(dialog_id, std::move(input_document->caption_), is_bot));

      td_->documents_manager_->create_document(file_id, thumbnail, std::move(file_name), std::move(mime_type), false);

      content = make_unique<MessageDocument>(file_id, std::move(caption));
      break;
    }
    case td_api::inputMessagePhoto::ID: {
      auto input_photo = static_cast<td_api::inputMessagePhoto *>(input_message_content.get());

      TRY_RESULT(caption, process_input_caption(dialog_id, std::move(input_photo->caption_), is_bot));
      if (input_photo->width_ < 0 || input_photo->width_ > 10000) {
        return Status::Error(400, "Wrong photo width");
      }
      if (input_photo->height_ < 0 || input_photo->height_ > 10000) {
        return Status::Error(400, "Wrong photo height");
      }
      ttl = input_photo->ttl_;

      auto message_photo = make_unique<MessagePhoto>();

      if (file_view.has_remote_location() && !file_view.remote_location().is_web()) {
        message_photo->photo.id = file_view.remote_location().get_id();
      }
      message_photo->photo.date = G()->unix_time();

      PhotoSize s;
      s.type = 'i';
      s.dimensions = get_dimensions(input_photo->width_, input_photo->height_);
      s.size = static_cast<int32>(file_view.size());
      s.file_id = file_id;

      if (thumbnail_file_id.is_valid()) {
        message_photo->photo.photos.push_back(thumbnail);
      }

      message_photo->photo.photos.push_back(s);

      message_photo->photo.has_stickers = !sticker_file_ids.empty();
      message_photo->photo.sticker_file_ids = std::move(sticker_file_ids);

      message_photo->caption = std::move(caption);

      content = std::move(message_photo);
      break;
    }
    case td_api::inputMessageSticker::ID: {
      auto input_sticker = static_cast<td_api::inputMessageSticker *>(input_message_content.get());
      td_->stickers_manager_->create_sticker(
          file_id, thumbnail, get_dimensions(input_sticker->width_, input_sticker->height_), true, nullptr, nullptr);

      content = make_unique<MessageSticker>(file_id);
      break;
    }
    case td_api::inputMessageVideo::ID: {
      auto input_video = static_cast<td_api::inputMessageVideo *>(input_message_content.get());

      TRY_RESULT(caption, process_input_caption(dialog_id, std::move(input_video->caption_), is_bot));
      ttl = input_video->ttl_;

      bool has_stickers = !sticker_file_ids.empty();
      td_->videos_manager_->create_video(file_id, thumbnail, has_stickers, std::move(sticker_file_ids),
                                         std::move(file_name), std::move(mime_type), input_video->duration_,
                                         get_dimensions(input_video->width_, input_video->height_),
                                         input_video->supports_streaming_, false);

      content = make_unique<MessageVideo>(file_id, std::move(caption));
      break;
    }
    case td_api::inputMessageVideoNote::ID: {
      auto input_video_note = static_cast<td_api::inputMessageVideoNote *>(input_message_content.get());

      auto length = input_video_note->length_;
      if (length < 0 || length >= 640) {
        return Status::Error(400, "Wrong video note length");
      }

      td_->video_notes_manager_->create_video_note(file_id, thumbnail, input_video_note->duration_,
                                                   get_dimensions(length, length), false);

      content = make_unique<MessageVideoNote>(file_id, false);
      break;
    }
    case td_api::inputMessageVoiceNote::ID: {
      auto input_voice_note = static_cast<td_api::inputMessageVoiceNote *>(input_message_content.get());

      TRY_RESULT(caption, process_input_caption(dialog_id, std::move(input_voice_note->caption_), is_bot));

      td_->voice_notes_manager_->create_voice_note(file_id, std::move(mime_type), input_voice_note->duration_,
                                                   std::move(input_voice_note->waveform_), false);

      content = make_unique<MessageVoiceNote>(file_id, std::move(caption), false);
      break;
    }
    case td_api::inputMessageLocation::ID: {
      TRY_RESULT(location, process_input_message_location(std::move(input_message_content)));
      if (location.second == 0) {
        content = make_unique<MessageLocation>(std::move(location.first));
      } else {
        content = make_unique<MessageLiveLocation>(std::move(location.first), location.second);
      }
      break;
    }
    case td_api::inputMessageVenue::ID: {
      TRY_RESULT(venue, process_input_message_venue(std::move(input_message_content)));
      content = make_unique<MessageVenue>(std::move(venue));
      break;
    }
    case td_api::inputMessageContact::ID: {
      TRY_RESULT(contact, process_input_message_contact(std::move(input_message_content)));
      content = make_unique<MessageContact>(std::move(contact));
      break;
    }
    case td_api::inputMessageGame::ID: {
      TRY_RESULT(game, process_input_message_game(std::move(input_message_content)));
      via_bot_user_id = game.get_bot_user_id();
      if (via_bot_user_id == td_->contacts_manager_->get_my_id("send_message")) {
        via_bot_user_id = UserId();
      }

      content = make_unique<MessageGame>(std::move(game));
      break;
    }
    case td_api::inputMessageInvoice::ID: {
      if (!is_bot) {
        return Status::Error(400, "Invoices can be sent only by bots");
      }

      auto input_message_invoice = move_tl_object_as<td_api::inputMessageInvoice>(input_message_content);
      if (!clean_input_string(input_message_invoice->title_)) {
        return Status::Error(400, "Invoice title must be encoded in UTF-8");
      }
      if (!clean_input_string(input_message_invoice->description_)) {
        return Status::Error(400, "Invoice description must be encoded in UTF-8");
      }
      if (!clean_input_string(input_message_invoice->photo_url_)) {
        return Status::Error(400, "Invoice photo URL must be encoded in UTF-8");
      }
      if (!clean_input_string(input_message_invoice->start_parameter_)) {
        return Status::Error(400, "Invoice bot start parameter must be encoded in UTF-8");
      }
      if (!clean_input_string(input_message_invoice->provider_token_)) {
        return Status::Error(400, "Invoice provider token must be encoded in UTF-8");
      }
      if (!clean_input_string(input_message_invoice->provider_data_)) {
        return Status::Error(400, "Invoice provider data must be encoded in UTF-8");
      }
      if (!clean_input_string(input_message_invoice->invoice_->currency_)) {
        return Status::Error(400, "Invoice currency must be encoded in UTF-8");
      }

      auto message_invoice = make_unique<MessageInvoice>();
      message_invoice->title = std::move(input_message_invoice->title_);
      message_invoice->description = std::move(input_message_invoice->description_);

      auto r_http_url = parse_url(input_message_invoice->photo_url_);
      if (r_http_url.is_error()) {
        message_invoice->photo.id = -2;
      } else {
        auto url = r_http_url.ok().get_url();
        auto r_invoice_file_id = td_->file_manager_->from_persistent_id(url, FileType::Temp);
        if (r_invoice_file_id.is_error()) {
          LOG(ERROR) << "Can't register url " << url;
          message_invoice->photo.id = -2;
        } else {
          auto invoice_file_id = r_invoice_file_id.move_as_ok();

          PhotoSize s;
          s.type = 'u';
          s.dimensions = get_dimensions(input_message_invoice->photo_width_, input_message_invoice->photo_height_);
          s.size = input_message_invoice->photo_size_;  // TODO use invoice_file_id size
          s.file_id = invoice_file_id;

          message_invoice->photo.photos.push_back(s);
        }
      }
      message_invoice->start_parameter = std::move(input_message_invoice->start_parameter_);

      message_invoice->invoice.currency = std::move(input_message_invoice->invoice_->currency_);
      message_invoice->invoice.price_parts.reserve(input_message_invoice->invoice_->price_parts_.size());
      int64 total_amount = 0;
      const int64 MAX_AMOUNT = 9999'9999'9999;
      for (auto &price : input_message_invoice->invoice_->price_parts_) {
        if (!clean_input_string(price->label_)) {
          return Status::Error(400, "Invoice price label must be encoded in UTF-8");
        }
        message_invoice->invoice.price_parts.emplace_back(std::move(price->label_), price->amount_);
        if (price->amount_ < -MAX_AMOUNT || price->amount_ > MAX_AMOUNT) {
          return Status::Error(400, "Too big amount of currency specified");
        }
        total_amount += price->amount_;
      }
      if (total_amount <= 0) {
        return Status::Error(400, "Total price must be positive");
      }
      if (total_amount > MAX_AMOUNT) {
        return Status::Error(400, "Total price is too big");
      }
      message_invoice->total_amount = total_amount;

      message_invoice->invoice.is_test = input_message_invoice->invoice_->is_test_;
      message_invoice->invoice.need_name = input_message_invoice->invoice_->need_name_;
      message_invoice->invoice.need_phone_number = input_message_invoice->invoice_->need_phone_number_;
      message_invoice->invoice.need_email_address = input_message_invoice->invoice_->need_email_address_;
      message_invoice->invoice.need_shipping_address = input_message_invoice->invoice_->need_shipping_address_;
      message_invoice->invoice.send_phone_number_to_provider =
          input_message_invoice->invoice_->send_phone_number_to_provider_;
      message_invoice->invoice.send_email_address_to_provider =
          input_message_invoice->invoice_->send_email_address_to_provider_;
      message_invoice->invoice.is_flexible = input_message_invoice->invoice_->is_flexible_;
      if (message_invoice->invoice.send_phone_number_to_provider) {
        message_invoice->invoice.need_phone_number = true;
      }
      if (message_invoice->invoice.send_email_address_to_provider) {
        message_invoice->invoice.need_email_address = true;
      }
      if (message_invoice->invoice.is_flexible) {
        message_invoice->invoice.need_shipping_address = true;
      }

      message_invoice->payload = std::move(input_message_invoice->payload_);
      message_invoice->provider_token = std::move(input_message_invoice->provider_token_);
      message_invoice->provider_data = std::move(input_message_invoice->provider_data_);

      content = std::move(message_invoice);
      break;
    }
    default:
      UNREACHABLE();
  }
  if (ttl < 0 || ttl > MAX_PRIVATE_MESSAGE_TTL) {
    return Status::Error(10, "Wrong message TTL specified");
  }
  if (ttl > 0 && dialog_id.get_type() != DialogType::User) {
    return Status::Error(10, "Message TTL can be specified only in private chats");
  }

  TRY_STATUS(can_send_message_content(dialog_id, content.get(), false));

  return InputMessageContent{std::move(content), disable_web_page_preview, clear_draft, ttl, via_bot_user_id};
}

Result<vector<MessageId>> MessagesManager::send_message_group(
    DialogId dialog_id, MessageId reply_to_message_id, bool disable_notification, bool from_background,
    vector<tl_object_ptr<td_api::InputMessageContent>> &&input_message_contents) {
  if (input_message_contents.size() > MAX_GROUPED_MESSAGES) {
    return Status::Error(4, "Too much messages to send as an album");
  }
  if (input_message_contents.empty()) {
    return Status::Error(4, "There is no messages to send");
  }

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(5, "Chat not found");
  }

  TRY_STATUS(can_send_message(dialog_id));

  vector<std::pair<unique_ptr<MessageContent>, int32>> message_contents;
  for (auto &input_message_content : input_message_contents) {
    TRY_RESULT(message_content, process_input_message_content(dialog_id, std::move(input_message_content)));
    if (!is_allowed_media_group_content(message_content.content->get_id())) {
      return Status::Error(5, "Wrong message content type");
    }

    message_contents.emplace_back(std::move(message_content.content), message_content.ttl);
  }

  reply_to_message_id = get_reply_to_message_id(d, reply_to_message_id);

  int64 media_album_id = 0;
  if (message_contents.size() > 1) {
    do {
      media_album_id = Random::secure_int64();
    } while (media_album_id >= 0 || pending_message_group_sends_.count(media_album_id) != 0);
  }

  // there must be no errors after get_message_to_send calls

  vector<MessageId> result;
  bool need_update_dialog_pos = false;
  for (auto &message_content : message_contents) {
    Message *m = get_message_to_send(d, reply_to_message_id, disable_notification, from_background,
                                     dup_message_content(dialog_id, message_content.first.get(), false),
                                     &need_update_dialog_pos);
    result.push_back(m->message_id);
    auto ttl = message_content.second;
    if (ttl > 0) {
      m->ttl = ttl;
      m->is_content_secret = is_secret_message_content(m->ttl, m->content->get_id());
    }
    m->media_album_id = media_album_id;

    save_send_message_logevent(dialog_id, m);
    do_send_message(dialog_id, m);

    send_update_new_message(d, m, true);
  }

  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "send_message_group");
  }

  return result;
}

void MessagesManager::save_send_message_logevent(DialogId dialog_id, Message *m) {
  if (!G()->parameters().use_message_db) {
    return;
  }

  CHECK(m != nullptr);
  LOG(INFO) << "Save " << FullMessageId(dialog_id, m->message_id) << " to binlog";
  auto logevent = SendMessageLogEvent(dialog_id, m);
  auto storer = LogEventStorerImpl<SendMessageLogEvent>(logevent);
  CHECK(m->send_message_logevent_id == 0);
  m->send_message_logevent_id =
      BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::SendMessage, storer);
}

void MessagesManager::do_send_message(DialogId dialog_id, Message *m, vector<int> bad_parts) {
  LOG(INFO) << "Do send " << FullMessageId(dialog_id, m->message_id);
  bool is_secret = dialog_id.get_type() == DialogType::SecretChat;

  if (m->media_album_id != 0 && bad_parts.empty() && !is_secret) {
    auto &request = pending_message_group_sends_[m->media_album_id];
    request.dialog_id = dialog_id;
    request.message_ids.push_back(m->message_id);
    request.is_finished.push_back(false);

    request.results.push_back(Status::OK());
  }

  auto content = m->content.get();
  auto content_type = content->get_id();
  if (content_type == MessageText::ID) {
    auto message_text = static_cast<const MessageText *>(content);

    int64 random_id = begin_send_message(dialog_id, m);
    if (is_secret) {
      auto layer = td_->contacts_manager_->get_secret_chat_layer(dialog_id.get_secret_chat_id());
      send_closure(td_->create_net_actor<SendSecretMessageActor>(), &SendSecretMessageActor::send, dialog_id,
                   m->reply_to_random_id, m->ttl, message_text->text.text,
                   get_secret_input_media(content, nullptr, BufferSlice(), layer),
                   get_input_secret_message_entities(message_text->text.entities), m->via_bot_user_id,
                   m->media_album_id, random_id);
    } else {
      send_closure(td_->create_net_actor<SendMessageActor>(), &SendMessageActor::send, get_message_flags(m), dialog_id,
                   m->reply_to_message_id, get_input_reply_markup(m->reply_markup),
                   get_input_message_entities(td_->contacts_manager_.get(), message_text->text.entities),
                   message_text->text.text, random_id, &m->send_query_ref,
                   get_sequence_dispatcher_id(dialog_id, content_type));
    }
    return;
  }

  FileId file_id = get_message_content_file_id(content);
  FileView file_view = td_->file_manager_->get_file_view(file_id);
  FileId thumbnail_file_id = get_message_content_thumbnail_file_id(content);
  if (is_secret) {
    auto layer = td_->contacts_manager_->get_secret_chat_layer(dialog_id.get_secret_chat_id());
    auto secret_input_media = get_secret_input_media(content, nullptr, BufferSlice(), layer);
    if (secret_input_media.empty()) {
      CHECK(file_view.is_encrypted());
      CHECK(file_id.is_valid());
      being_uploaded_files_[file_id] = {FullMessageId(dialog_id, m->message_id), thumbnail_file_id};
      LOG(INFO) << "Ask to upload encrypted file " << file_id;
      // need to call resume_upload synchronously to make upload process consistent with being_uploaded_files_
      td_->file_manager_->resume_upload(file_id, std::move(bad_parts), upload_media_callback_, 1, m->message_id.get());
    } else {
      on_secret_message_media_uploaded(dialog_id, m, std::move(secret_input_media), FileId(), FileId());
    }
  } else {
    auto input_media = get_input_media(content, nullptr, nullptr, m->ttl);
    if (input_media == nullptr) {
      if (content_type == MessagePhoto::ID) {
        thumbnail_file_id = FileId();
      }

      CHECK(file_id.is_valid());
      being_uploaded_files_[file_id] = {FullMessageId(dialog_id, m->message_id), thumbnail_file_id};
      LOG(INFO) << "Ask to upload file " << file_id;
      // need to call resume_upload synchronously to make upload process consistent with being_uploaded_files_
      td_->file_manager_->resume_upload(file_id, std::move(bad_parts), upload_media_callback_, 1, m->message_id.get());
    } else {
      on_message_media_uploaded(dialog_id, m, std::move(input_media), FileId(), FileId());
    }
  }
}

void MessagesManager::on_message_media_uploaded(DialogId dialog_id, Message *m,
                                                tl_object_ptr<telegram_api::InputMedia> &&input_media, FileId file_id,
                                                FileId thumbnail_file_id) {
  CHECK(m != nullptr);
  CHECK(input_media != nullptr);
  if (m->media_album_id == 0) {
    on_media_message_ready_to_send(
        dialog_id, m->message_id,
        PromiseCreator::lambda([this, dialog_id, input_media = std::move(input_media), file_id,
                                thumbnail_file_id](Result<Message *> result) mutable {
          if (result.is_error() || G()->close_flag()) {
            return;
          }

          auto m = result.move_as_ok();
          CHECK(m != nullptr);
          CHECK(input_media != nullptr);

          auto caption = get_message_content_caption(m->content.get());

          LOG(INFO) << "Send media from " << m->message_id << " in " << dialog_id << " in reply to "
                    << m->reply_to_message_id;
          int64 random_id = begin_send_message(dialog_id, m);
          send_closure(td_->create_net_actor<SendMediaActor>(), &SendMediaActor::send, file_id, thumbnail_file_id,
                       get_message_flags(m), dialog_id, m->reply_to_message_id, get_input_reply_markup(m->reply_markup),
                       get_input_message_entities(td_->contacts_manager_.get(), caption.entities), caption.text,
                       std::move(input_media), random_id, &m->send_query_ref,
                       get_sequence_dispatcher_id(dialog_id, m->content->get_id()));
        }));
  } else {
    switch (input_media->get_id()) {
      case telegram_api::inputMediaUploadedDocument::ID:
        static_cast<telegram_api::inputMediaUploadedDocument *>(input_media.get())->flags_ |=
            telegram_api::inputMediaUploadedDocument::NOSOUND_VIDEO_MASK;
      // fallthrough
      case telegram_api::inputMediaUploadedPhoto::ID:
      case telegram_api::inputMediaDocumentExternal::ID:
      case telegram_api::inputMediaPhotoExternal::ID:
        LOG(INFO) << "Upload media from " << m->message_id << " in " << dialog_id;
        td_->create_handler<UploadMediaQuery>()->send(dialog_id, m->message_id, file_id, thumbnail_file_id,
                                                      std::move(input_media));
        break;
      case telegram_api::inputMediaDocument::ID:
      case telegram_api::inputMediaPhoto::ID:
        send_closure_later(actor_id(this), &MessagesManager::on_upload_message_media_finished, m->media_album_id,
                           dialog_id, m->message_id, Status::OK());
        break;
      default:
        LOG(ERROR) << "Have wrong input media " << to_string(input_media);
        send_closure_later(actor_id(this), &MessagesManager::on_upload_message_media_finished, m->media_album_id,
                           dialog_id, m->message_id, Status::Error(400, "Wrong input media"));
    }
  }
}

void MessagesManager::on_secret_message_media_uploaded(DialogId dialog_id, Message *m,
                                                       SecretInputMedia &&secret_input_media, FileId file_id,
                                                       FileId thumbnail_file_id) {
  CHECK(m != nullptr);
  CHECK(!secret_input_media.empty());
  /*
  if (m->media_album_id != 0) {
    switch (secret_input_media->input_file_->get_id()) {
      case telegram_api::inputEncryptedFileUploaded::ID:
      case telegram_api::inputEncryptedFileBigUploaded::ID:
        LOG(INFO) << "Upload media from " << m->message_id << " in " << dialog_id;
        return td_->create_handler<UploadEncryptedMediaQuery>()->send(dialog_id, m->message_id, std::move(secret_input_media));
      case telegram_api::inputEncryptedFile::ID:
        return send_closure_later(actor_id(this), &MessagesManager::on_upload_message_media_finished, m->media_album_id,
                           dialog_id, m->message_id, Status::OK());
      default:
        LOG(ERROR) << "Have wrong secret input media " << to_string(secret_input_media->input_file_);
        return send_closure_later(actor_id(this), &MessagesManager::on_upload_message_media_finished, m->media_album_id,
                           dialog_id, m->message_id, Status::Error(400, "Wrong input media"));
  }
*/
  // TODO use file_id, thumbnail_file_id, invalidate partial remote location for file_id in case of failed upload
  // even message has already been deleted
  on_media_message_ready_to_send(
      dialog_id, m->message_id,
      PromiseCreator::lambda(
          [this, dialog_id, secret_input_media = std::move(secret_input_media)](Result<Message *> result) mutable {
            if (result.is_error() || G()->close_flag()) {
              return;
            }

            auto m = result.move_as_ok();
            CHECK(m != nullptr);
            CHECK(!secret_input_media.empty());
            LOG(INFO) << "Send secret media from " << m->message_id << " in " << dialog_id << " in reply to "
                      << m->reply_to_message_id;
            int64 random_id = begin_send_message(dialog_id, m);
            send_closure(td_->create_net_actor<SendSecretMessageActor>(), &SendSecretMessageActor::send, dialog_id,
                         m->reply_to_random_id, m->ttl, "", std::move(secret_input_media),
                         vector<tl_object_ptr<secret_api::MessageEntity>>(), m->via_bot_user_id, m->media_album_id,
                         random_id);
          }));
}

void MessagesManager::on_upload_message_media_success(DialogId dialog_id, MessageId message_id,
                                                      tl_object_ptr<telegram_api::MessageMedia> &&media) {
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  Message *m = get_message(d, message_id);
  if (m == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel
    // don't need to send error to the user, because the message has already been deleted
    // and there is nothing to be deleted from the server
    LOG(INFO) << "Fail to send already deleted by the user or sent to inaccessible chat "
              << FullMessageId{dialog_id, message_id};
    return;
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    return;  // the message should be deleted soon
  }

  auto content = get_message_content(get_message_content_caption(m->content.get()), std::move(media), dialog_id, false,
                                     UserId(), nullptr);

  update_message_content(dialog_id, m, m->content, std::move(content), true);

  auto input_media = get_input_media(m->content.get(), nullptr, nullptr, m->ttl);
  Status result;
  if (input_media == nullptr) {
    result = Status::Error(400, "Failed to upload file");
  }

  send_closure_later(actor_id(this), &MessagesManager::on_upload_message_media_finished, m->media_album_id, dialog_id,
                     message_id, std::move(result));
}

void MessagesManager::on_upload_message_media_file_part_missing(DialogId dialog_id, MessageId message_id,
                                                                int bad_part) {
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  Message *m = get_message(d, message_id);
  if (m == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel
    // don't need to send error to the user, because the message has already been deleted
    // and there is nothing to be deleted from the server
    LOG(INFO) << "Fail to send already deleted by the user or sent to inaccessible chat "
              << FullMessageId{dialog_id, message_id};
    return;
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // LOG(ERROR) << "Found " << m->message_id << " in inaccessible " << dialog_id;
    // dump_debug_message_op(get_dialog(dialog_id), 5);
    return;  // the message should be deleted soon
  }

  CHECK(dialog_id.get_type() != DialogType::SecretChat);

  do_send_message(dialog_id, m, {bad_part});
}

void MessagesManager::on_upload_message_media_fail(DialogId dialog_id, MessageId message_id, Status error) {
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  Message *m = get_message(d, message_id);
  if (m == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel
    // don't need to send error to the user, because the message has already been deleted
    // and there is nothing to be deleted from the server
    LOG(INFO) << "Fail to send already deleted by the user or sent to inaccessible chat "
              << FullMessageId{dialog_id, message_id};
    return;
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // LOG(ERROR) << "Found " << m->message_id << " in inaccessible " << dialog_id;
    // dump_debug_message_op(get_dialog(dialog_id), 5);
    return;  // the message should be deleted soon
  }

  CHECK(dialog_id.get_type() != DialogType::SecretChat);

  send_closure_later(actor_id(this), &MessagesManager::on_upload_message_media_finished, m->media_album_id, dialog_id,
                     message_id, std::move(error));
}

void MessagesManager::on_upload_message_media_finished(int64 media_album_id, DialogId dialog_id, MessageId message_id,
                                                       Status result) {
  CHECK(media_album_id < 0);
  auto it = pending_message_group_sends_.find(media_album_id);
  if (it == pending_message_group_sends_.end()) {
    // the group may be already sent or failed to be sent
    return;
  }
  auto &request = it->second;
  CHECK(request.dialog_id == dialog_id);
  auto message_it = std::find(request.message_ids.begin(), request.message_ids.end(), message_id);
  CHECK(message_it != request.message_ids.end());
  auto pos = static_cast<size_t>(message_it - request.message_ids.begin());

  if (request.is_finished[pos]) {
    return;
  }
  LOG(INFO) << "Finish to upload media of " << message_id << " in " << dialog_id << " from group " << media_album_id;

  request.results[pos] = std::move(result);
  request.is_finished[pos] = true;
  request.finished_count++;

  if (request.finished_count == request.message_ids.size() || request.results[pos].is_error()) {
    // send later, because some messages may be being deleted now
    for (auto request_message_id : request.message_ids) {
      LOG(INFO) << "Send on_media_message_ready_to_send for " << request_message_id << " in " << dialog_id;
      send_closure_later(actor_id(this), &MessagesManager::on_media_message_ready_to_send, dialog_id,
                         request_message_id,
                         PromiseCreator::lambda([actor_id = actor_id(this)](Result<Message *> result) mutable {
                           if (result.is_error() || G()->close_flag()) {
                             return;
                           }

                           auto m = result.move_as_ok();
                           CHECK(m != nullptr);
                           send_closure_later(actor_id, &MessagesManager::do_send_message_group, m->media_album_id);
                         }));
    }
  }
}

void MessagesManager::do_send_message_group(int64 media_album_id) {
  CHECK(media_album_id < 0);
  auto it = pending_message_group_sends_.find(media_album_id);
  if (it == pending_message_group_sends_.end()) {
    // the group may be already sent or failed to be sent
    return;
  }
  auto &request = it->second;

  auto dialog_id = request.dialog_id;
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  auto default_status = can_send_message(dialog_id);
  bool success = default_status.is_ok();
  vector<int64> random_ids;
  vector<tl_object_ptr<telegram_api::inputSingleMedia>> input_single_media;
  MessageId reply_to_message_id;
  int32 flags = 0;
  for (size_t i = 0; i < request.message_ids.size(); i++) {
    auto *m = get_message(d, request.message_ids[i]);
    if (m == nullptr) {
      // skip deleted messages
      random_ids.push_back(0);
      continue;
    }

    reply_to_message_id = m->reply_to_message_id;
    flags = get_message_flags(m);

    random_ids.push_back(begin_send_message(dialog_id, m));
    auto caption = get_message_content_caption(m->content.get());
    auto input_media = get_input_media(m->content.get(), nullptr, nullptr, m->ttl);
    auto entities = get_input_message_entities(td_->contacts_manager_.get(), caption.entities);
    int32 input_single_media_flags = 0;
    if (!entities.empty()) {
      input_single_media_flags |= telegram_api::inputSingleMedia::ENTITIES_MASK;
    }

    input_single_media.push_back(make_tl_object<telegram_api::inputSingleMedia>(
        input_single_media_flags, std::move(input_media), random_ids.back(), caption.text, std::move(entities)));
    if (request.results[i].is_error()) {
      success = false;
    }
  }

  if (!success) {
    if (default_status.is_ok()) {
      default_status = Status::Error(400, "Group send failed");
    }
    for (size_t i = 0; i < random_ids.size(); i++) {
      if (random_ids[i] != 0) {
        on_send_message_fail(random_ids[i],
                             request.results[i].is_error() ? std::move(request.results[i]) : default_status.clone());
      }
    }
    pending_message_group_sends_.erase(it);
    return;
  }
  CHECK(request.finished_count == request.message_ids.size());
  pending_message_group_sends_.erase(it);

  LOG(INFO) << "Begin to send media group " << media_album_id << " to " << dialog_id;

  if (input_single_media.empty()) {
    LOG(INFO) << "Media group " << media_album_id << " from " << dialog_id << " is empty";
  }
  send_closure(td_->create_net_actor<SendMultiMediaActor>(), &SendMultiMediaActor::send, flags, dialog_id,
               reply_to_message_id, std::move(input_single_media),
               get_sequence_dispatcher_id(dialog_id, MessagePhoto::ID));
}

void MessagesManager::on_media_message_ready_to_send(DialogId dialog_id, MessageId message_id,
                                                     Promise<Message *> &&promise) {
  LOG(INFO) << "Ready to send " << message_id << " to " << dialog_id;
  CHECK(promise);
  if (!G()->parameters().use_file_db) {  // ResourceManager::Mode::Greedy
    auto m = get_message({dialog_id, message_id});
    if (m != nullptr) {
      promise.set_value(std::move(m));
    }
    return;
  }

  auto queue_id = get_sequence_dispatcher_id(dialog_id, MessagePhoto::ID);
  CHECK(queue_id & 1);
  auto &queue = yet_unsent_media_queues_[queue_id];
  auto it = queue.find(message_id.get());
  if (it == queue.end()) {
    if (queue.empty()) {
      yet_unsent_media_queues_.erase(queue_id);
    }

    LOG(INFO) << "Can't find " << message_id << " in the queue of " << dialog_id;
    auto m = get_message({dialog_id, message_id});
    if (m != nullptr) {
      promise.set_value(std::move(m));
    }
    return;
  }
  if (it->second) {
    promise.set_error(Status::Error(500, "Duplicate promise"));
    return;
  }
  it->second = std::move(promise);

  on_yet_unsent_media_queue_updated(dialog_id);
}

void MessagesManager::on_yet_unsent_media_queue_updated(DialogId dialog_id) {
  auto queue_id = get_sequence_dispatcher_id(dialog_id, MessagePhoto::ID);
  CHECK(queue_id & 1);
  auto &queue = yet_unsent_media_queues_[queue_id];
  LOG(INFO) << "Queue for " << dialog_id << " is updated to size of " << queue.size();
  while (!queue.empty()) {
    auto first_it = queue.begin();
    if (!first_it->second) {
      break;
    }

    auto m = get_message({dialog_id, MessageId(first_it->first)});
    if (m != nullptr) {
      LOG(INFO) << "Can send " << FullMessageId{dialog_id, m->message_id};
      first_it->second.set_value(std::move(m));
    }
    queue.erase(first_it);
  }
  LOG(INFO) << "Queue for " << dialog_id << " now has size " << queue.size();
  if (queue.empty()) {
    yet_unsent_media_queues_.erase(queue_id);
  }
}

Result<MessageId> MessagesManager::send_bot_start_message(UserId bot_user_id, DialogId dialog_id,
                                                          const string &parameter) {
  LOG(INFO) << "Begin to send bot start message to " << dialog_id;
  if (td_->auth_manager_->is_bot()) {
    return Status::Error(5, "Bot can't send start message to another bot");
  }

  TRY_RESULT(bot_data, td_->contacts_manager_->get_bot_data(bot_user_id));

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(5, "Chat not found");
  }

  bool is_chat_with_bot = false;
  switch (dialog_id.get_type()) {
    case DialogType::User:
      if (dialog_id.get_user_id() != bot_user_id) {
        return Status::Error(5, "Can't send start message to a private chat other than chat with the bot");
      }
      is_chat_with_bot = true;
      break;
    case DialogType::Chat: {
      if (!bot_data.can_join_groups) {
        return Status::Error(5, "Bot can't join groups");
      }

      auto chat_id = dialog_id.get_chat_id();
      if (!td_->contacts_manager_->have_input_peer_chat(chat_id, AccessRights::Write)) {
        return Status::Error(3, "Can't access the chat");
      }
      auto status = td_->contacts_manager_->get_chat_status(chat_id);
      if (!status.can_invite_users()) {
        return Status::Error(3, "Need administrator rights to invite a bot to the group chat");
      }
      break;
    }
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      if (!td_->contacts_manager_->have_input_peer_channel(channel_id, AccessRights::Write)) {
        return Status::Error(3, "Can't access the chat");
      }
      switch (td_->contacts_manager_->get_channel_type(channel_id)) {
        case ChannelType::Megagroup:
          if (!bot_data.can_join_groups) {
            return Status::Error(5, "The bot can't join groups");
          }
          break;
        case ChannelType::Broadcast:
          return Status::Error(3, "Bots can't be invited to channel chats. Add them as administrators instead");
        case ChannelType::Unknown:
        default:
          UNREACHABLE();
      }
      auto status = td_->contacts_manager_->get_channel_status(channel_id);
      if (!status.can_invite_users()) {
        return Status::Error(3, "Need administrator rights to invite a bot to the supergroup chat");
      }
      break;
    }
    case DialogType::SecretChat:
      return Status::Error(5, "Can't send bot start message to a secret chat");
    case DialogType::None:
    default:
      UNREACHABLE();
  }
  string text = "/start";
  if (!is_chat_with_bot) {
    text += '@';
    text += bot_data.username;
  }

  bool need_update_dialog_pos = false;
  Message *m = get_message_to_send(
      d, MessageId(), false, false,
      make_unique<MessageText>(
          FormattedText{text, vector<MessageEntity>{MessageEntity(MessageEntity::Type::BotCommand, 0,
                                                                  narrow_cast<int32>(text.size()))}},
          WebPageId()),
      &need_update_dialog_pos);

  send_update_new_message(d, m, true);
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "send_bot_start_message");
  }

  save_send_bot_start_message_logevent(bot_user_id, dialog_id, parameter, m);
  do_send_bot_start_message(bot_user_id, dialog_id, parameter, m);
  return m->message_id;
}

class MessagesManager::SendBotStartMessageLogEvent {
 public:
  UserId bot_user_id;
  DialogId dialog_id;
  string parameter;
  const Message *m_in = nullptr;
  unique_ptr<Message> m_out;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(bot_user_id, storer);
    td::store(dialog_id, storer);
    td::store(parameter, storer);
    td::store(*m_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(bot_user_id, parser);
    td::parse(dialog_id, parser);
    td::parse(parameter, parser);
    CHECK(m_out == nullptr);
    m_out = make_unique<Message>();
    td::parse(*m_out, parser);
  }
};

void MessagesManager::save_send_bot_start_message_logevent(UserId bot_user_id, DialogId dialog_id,
                                                           const string &parameter, Message *m) {
  if (!G()->parameters().use_message_db) {
    return;
  }

  CHECK(m != nullptr);
  LOG(INFO) << "Save " << FullMessageId(dialog_id, m->message_id) << " to binlog";
  SendBotStartMessageLogEvent logevent;
  logevent.bot_user_id = bot_user_id;
  logevent.dialog_id = dialog_id;
  logevent.parameter = parameter;
  logevent.m_in = m;
  auto storer = LogEventStorerImpl<SendBotStartMessageLogEvent>(logevent);
  CHECK(m->send_message_logevent_id == 0);
  m->send_message_logevent_id =
      BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::SendBotStartMessage, storer);
}

void MessagesManager::do_send_bot_start_message(UserId bot_user_id, DialogId dialog_id, const string &parameter,
                                                Message *m) {
  LOG(INFO) << "Do send bot start " << FullMessageId(dialog_id, m->message_id) << " to bot " << bot_user_id;

  int64 random_id = begin_send_message(dialog_id, m);
  telegram_api::object_ptr<telegram_api::InputPeer> input_peer = dialog_id.get_type() == DialogType::User
                                                                     ? make_tl_object<telegram_api::inputPeerEmpty>()
                                                                     : get_input_peer(dialog_id, AccessRights::Write);
  if (input_peer == nullptr) {
    return on_send_message_fail(random_id, Status::Error(400, "Have no info about the chat"));
  }
  auto bot_input_user = td_->contacts_manager_->get_input_user(bot_user_id);
  if (bot_input_user == nullptr) {
    return on_send_message_fail(random_id, Status::Error(400, "Have no info about the bot"));
  }

  m->send_query_ref = td_->create_handler<StartBotQuery>()->send(std::move(bot_input_user), dialog_id,
                                                                 std::move(input_peer), parameter, random_id);
}

Result<MessageId> MessagesManager::send_inline_query_result_message(DialogId dialog_id, MessageId reply_to_message_id,
                                                                    bool disable_notification, bool from_background,
                                                                    int64 query_id, const string &result_id) {
  LOG(INFO) << "Begin to send inline query result message to " << dialog_id << " in reply to " << reply_to_message_id;

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(5, "Chat not found");
  }

  TRY_STATUS(can_send_message(dialog_id));
  bool to_secret = false;
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      // ok
      break;
    case DialogType::Channel: {
      auto channel_status = td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id());
      if (!channel_status.can_use_inline_bots()) {
        return Status::Error(400, "Can't use inline bots in the chat");
      }
      break;
    }
    case DialogType::SecretChat:
      to_secret = true;
      // ok
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  const MessageContent *message_content;
  const ReplyMarkup *reply_markup;
  bool disable_web_page_preview;
  std::tie(message_content, reply_markup, disable_web_page_preview) =
      td_->inline_queries_manager_->get_inline_message_content(query_id, result_id);
  if (message_content == nullptr) {
    return Status::Error(5, "Inline query result not found");
  }

  TRY_STATUS(can_send_message_content(dialog_id, message_content, false));

  bool need_update_dialog_pos = false;
  Message *m =
      get_message_to_send(d, get_reply_to_message_id(d, reply_to_message_id), disable_notification, from_background,
                          dup_message_content(dialog_id, message_content, false), &need_update_dialog_pos);
  m->via_bot_user_id = td_->inline_queries_manager_->get_inline_bot_user_id(query_id);
  if (reply_markup != nullptr && !to_secret) {
    m->reply_markup = make_unique<ReplyMarkup>(*reply_markup);
  }
  m->disable_web_page_preview = disable_web_page_preview;
  m->clear_draft = true;

  update_dialog_draft_message(d, nullptr, false, !need_update_dialog_pos);

  send_update_new_message(d, m, true);
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "send_inline_query_result_message");
  }

  if (to_secret) {
    save_send_message_logevent(dialog_id, m);
    auto message_id = m->message_id;
    do_send_message(dialog_id, m);
    return message_id;
  }

  save_send_inline_query_result_message_logevent(dialog_id, m, query_id, result_id);
  do_send_inline_query_result_message(dialog_id, m, query_id, result_id);
  return m->message_id;
}

class MessagesManager::SendInlineQueryResultMessageLogEvent {
 public:
  DialogId dialog_id;
  int64 query_id;
  string result_id;
  const Message *m_in = nullptr;
  unique_ptr<Message> m_out;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id, storer);
    td::store(query_id, storer);
    td::store(result_id, storer);
    td::store(*m_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id, parser);
    td::parse(query_id, parser);
    td::parse(result_id, parser);
    CHECK(m_out == nullptr);
    m_out = make_unique<Message>();
    td::parse(*m_out, parser);
  }
};

void MessagesManager::save_send_inline_query_result_message_logevent(DialogId dialog_id, Message *m, int64 query_id,
                                                                     const string &result_id) {
  if (!G()->parameters().use_message_db) {
    return;
  }

  CHECK(m != nullptr);
  LOG(INFO) << "Save " << FullMessageId(dialog_id, m->message_id) << " to binlog";
  SendInlineQueryResultMessageLogEvent logevent;
  logevent.dialog_id = dialog_id;
  logevent.query_id = query_id;
  logevent.result_id = result_id;
  logevent.m_in = m;
  auto storer = LogEventStorerImpl<SendInlineQueryResultMessageLogEvent>(logevent);
  CHECK(m->send_message_logevent_id == 0);
  m->send_message_logevent_id =
      BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::SendInlineQueryResultMessage, storer);
}

void MessagesManager::do_send_inline_query_result_message(DialogId dialog_id, Message *m, int64 query_id,
                                                          const string &result_id) {
  LOG(INFO) << "Do send inline query result " << FullMessageId(dialog_id, m->message_id);

  int64 random_id = begin_send_message(dialog_id, m);
  m->send_query_ref = td_->create_handler<SendInlineBotResultQuery>()->send(
      get_message_flags(m), dialog_id, m->reply_to_message_id, random_id, query_id, result_id);
}

bool MessagesManager::can_edit_message(DialogId dialog_id, const Message *m, bool is_editing,
                                       bool only_reply_markup) const {
  if (m == nullptr) {
    return false;
  }
  if (m->message_id.is_yet_unsent()) {
    return false;
  }
  if (m->message_id.is_local()) {
    return false;
  }
  if (m->forward_info != nullptr) {
    return false;
  }

  bool is_bot = td_->auth_manager_->is_bot();
  if (m->had_reply_markup) {
    return false;
  }
  if (!is_bot && m->reply_markup != nullptr) {
    return false;
  }
  if (m->reply_markup != nullptr && m->reply_markup->type != ReplyMarkup::Type::InlineKeyboard) {
    return false;
  }

  auto my_id = td_->contacts_manager_->get_my_id("can_edit_message");
  if (m->via_bot_user_id.is_valid() && m->via_bot_user_id != my_id) {
    return false;
  }

  DialogId my_dialog_id(my_id);
  bool has_edit_time_limit = !(is_bot && m->is_outgoing) && dialog_id != my_dialog_id;

  switch (dialog_id.get_type()) {
    case DialogType::User:
      if (!m->is_outgoing && dialog_id != my_dialog_id) {
        return false;
      }
      break;
    case DialogType::Chat:
      if (!m->is_outgoing) {
        return false;
      }
      break;
    case DialogType::Channel: {
      if (m->via_bot_user_id.is_valid()) {
        // outgoing via_bot messages can always be edited
        break;
      }

      auto channel_id = dialog_id.get_channel_id();
      auto channel_status = td_->contacts_manager_->get_channel_status(channel_id);
      if (m->is_channel_post) {
        if (!channel_status.can_edit_messages() && !(channel_status.can_post_messages() && m->is_outgoing)) {
          return false;
        }
      } else {
        if (!m->is_outgoing) {
          return false;
        }
        if (channel_status.can_pin_messages()) {
          has_edit_time_limit = false;
        }
      }
      break;
    }
    case DialogType::SecretChat:
      return false;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }

  const int32 DEFAULT_EDIT_TIME_LIMIT = 2 * 86400;
  int32 edit_time_limit = G()->shared_config().get_option_integer("edit_time_limit", DEFAULT_EDIT_TIME_LIMIT);
  if (has_edit_time_limit && G()->unix_time_cached() - m->date >= edit_time_limit + (is_editing ? 300 : 0)) {
    return false;
  }

  switch (m->content->get_id()) {
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageDocument::ID:
    case MessageGame::ID:
    case MessagePhoto::ID:
    case MessageText::ID:
    case MessageVideo::ID:
    case MessageVoiceNote::ID:
      return true;
    case MessageLiveLocation::ID: {
      if (td_->auth_manager_->is_bot() && only_reply_markup) {
        // there is no caption to edit, but bot can edit inline reply_markup
        return true;
      }
      return G()->unix_time_cached() - m->date < static_cast<const MessageLiveLocation *>(m->content.get())->period;
    }
    case MessageContact::ID:
    case MessageLocation::ID:
    case MessageSticker::ID:
    case MessageVenue::ID:
    case MessageVideoNote::ID:
      // there is no caption to edit, but bot can edit inline reply_markup
      return td_->auth_manager_->is_bot() && only_reply_markup;
    case MessageInvoice::ID:
    case MessageUnsupported::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return false;
    default:
      UNREACHABLE();
  }

  return false;
}

bool MessagesManager::is_broadcast_channel(DialogId dialog_id) const {
  if (dialog_id.get_type() != DialogType::Channel) {
    return false;
  }

  return td_->contacts_manager_->get_channel_type(dialog_id.get_channel_id()) == ChannelType::Broadcast;
}

void MessagesManager::edit_message_text(FullMessageId full_message_id,
                                        tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                        tl_object_ptr<td_api::InputMessageContent> &&input_message_content,
                                        Promise<Unit> &&promise) {
  if (input_message_content == nullptr) {
    return promise.set_error(Status::Error(5, "Can't edit message without new content"));
  }
  int32 new_message_content_type = input_message_content->get_id();
  if (new_message_content_type != td_api::inputMessageText::ID) {
    return promise.set_error(Status::Error(5, "Input message content type must be InputMessageText"));
  }

  LOG(INFO) << "Begin to edit text of " << full_message_id;
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(5, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Edit)) {
    return promise.set_error(Status::Error(5, "Can't access the chat"));
  }

  auto message_id = full_message_id.get_message_id();
  const Message *message = get_message_force(d, message_id);
  if (message == nullptr) {
    return promise.set_error(Status::Error(5, "Message not found"));
  }

  if (!can_edit_message(dialog_id, message, true)) {
    return promise.set_error(Status::Error(5, "Message can't be edited"));
  }

  int32 old_message_content_type = message->content->get_id();
  if (old_message_content_type != MessageText::ID && old_message_content_type != MessageGame::ID) {
    return promise.set_error(Status::Error(5, "There is no text in the message to edit"));
  }

  auto r_input_message_text =
      process_input_message_text(dialog_id, std::move(input_message_content), td_->auth_manager_->is_bot());
  if (r_input_message_text.is_error()) {
    return promise.set_error(r_input_message_text.move_as_error());
  }
  InputMessageText input_message_text = r_input_message_text.move_as_ok();

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false,
                                             !is_broadcast_channel(dialog_id));
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }
  auto input_reply_markup = get_input_reply_markup(r_new_reply_markup.ok());
  int32 flags = 0;
  if (input_message_text.disable_web_page_preview) {
    flags |= SEND_MESSAGE_FLAG_DISABLE_WEB_PAGE_PREVIEW;
  }

  send_closure(td_->create_net_actor<EditMessageActor>(std::move(promise)), &EditMessageActor::send, flags, dialog_id,
               message_id, input_message_text.text.text,
               get_input_message_entities(td_->contacts_manager_.get(), input_message_text.text.entities), nullptr,
               std::move(input_reply_markup), get_sequence_dispatcher_id(dialog_id, -1));
}

void MessagesManager::edit_message_live_location(FullMessageId full_message_id,
                                                 tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                 tl_object_ptr<td_api::location> &&input_location,
                                                 Promise<Unit> &&promise) {
  LOG(INFO) << "Begin to edit live location of " << full_message_id;
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(5, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Edit)) {
    return promise.set_error(Status::Error(5, "Can't access the chat"));
  }

  auto message_id = full_message_id.get_message_id();
  const Message *message = get_message_force(d, message_id);
  if (message == nullptr) {
    return promise.set_error(Status::Error(5, "Message not found"));
  }

  if (!can_edit_message(dialog_id, message, true)) {
    return promise.set_error(Status::Error(5, "Message can't be edited"));
  }

  int32 old_message_content_type = message->content->get_id();
  if (old_message_content_type != MessageLiveLocation::ID) {
    return promise.set_error(Status::Error(5, "There is no live location in the message to edit"));
  }

  Location location(input_location);
  if (location.empty() && input_location != nullptr) {
    return promise.set_error(Status::Error(400, "Wrong location specified"));
  }

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false,
                                             !is_broadcast_channel(dialog_id));
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }
  auto input_reply_markup = get_input_reply_markup(r_new_reply_markup.ok());

  int32 flags = 0;
  if (location.empty()) {
    flags |= telegram_api::messages_editMessage::STOP_GEO_LIVE_MASK;
  }
  send_closure(td_->create_net_actor<EditMessageActor>(std::move(promise)), &EditMessageActor::send, flags, dialog_id,
               message_id, string(), vector<tl_object_ptr<telegram_api::MessageEntity>>(),
               location.empty() ? nullptr : location.get_input_geo_point(), std::move(input_reply_markup),
               get_sequence_dispatcher_id(dialog_id, -1));
}

void MessagesManager::edit_message_caption(FullMessageId full_message_id,
                                           tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                           tl_object_ptr<td_api::formattedText> &&input_caption,
                                           Promise<Unit> &&promise) {
  LOG(INFO) << "Begin to edit caption of " << full_message_id;

  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(5, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Edit)) {
    return promise.set_error(Status::Error(5, "Can't access the chat"));
  }

  auto message_id = full_message_id.get_message_id();
  const Message *message = get_message_force(d, message_id);
  if (message == nullptr) {
    return promise.set_error(Status::Error(5, "Message not found"));
  }

  if (!can_edit_message(dialog_id, message, true)) {
    return promise.set_error(Status::Error(5, "Message can't be edited"));
  }

  if (!can_have_message_content_caption(message->content->get_id())) {
    return promise.set_error(Status::Error(400, "There is no caption in the message to edit"));
  }

  auto r_caption = process_input_caption(dialog_id, std::move(input_caption), td_->auth_manager_->is_bot());
  if (r_caption.is_error()) {
    return promise.set_error(r_caption.move_as_error());
  }
  auto caption = r_caption.move_as_ok();

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false,
                                             !is_broadcast_channel(dialog_id));
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }
  auto input_reply_markup = get_input_reply_markup(r_new_reply_markup.ok());

  send_closure(td_->create_net_actor<EditMessageActor>(std::move(promise)), &EditMessageActor::send, 1 << 11, dialog_id,
               message_id, caption.text, get_input_message_entities(td_->contacts_manager_.get(), caption.entities),
               nullptr, std::move(input_reply_markup), get_sequence_dispatcher_id(dialog_id, -1));
}

void MessagesManager::edit_message_reply_markup(FullMessageId full_message_id,
                                                tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is available only for bots"));
  }

  LOG(INFO) << "Begin to edit reply markup of " << full_message_id;
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(5, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Edit)) {
    return promise.set_error(Status::Error(5, "Can't access the chat"));
  }

  auto message_id = full_message_id.get_message_id();
  const Message *message = get_message_force(d, message_id);
  if (message == nullptr) {
    return promise.set_error(Status::Error(5, "Message not found"));
  }

  if (!can_edit_message(dialog_id, message, true, true)) {
    return promise.set_error(Status::Error(5, "Message can't be edited"));
  }

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false,
                                             !is_broadcast_channel(dialog_id));
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }
  auto input_reply_markup = get_input_reply_markup(r_new_reply_markup.ok());
  send_closure(td_->create_net_actor<EditMessageActor>(std::move(promise)), &EditMessageActor::send, 0, dialog_id,
               message_id, string(), vector<tl_object_ptr<telegram_api::MessageEntity>>(), nullptr,
               std::move(input_reply_markup), get_sequence_dispatcher_id(dialog_id, -1));
}

void MessagesManager::edit_inline_message_text(const string &inline_message_id,
                                               tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                               tl_object_ptr<td_api::InputMessageContent> &&input_message_content,
                                               Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is available only for bots"));
  }

  if (input_message_content == nullptr) {
    return promise.set_error(Status::Error(5, "Can't edit message without new content"));
  }
  int32 new_message_content_type = input_message_content->get_id();
  if (new_message_content_type != td_api::inputMessageText::ID) {
    return promise.set_error(Status::Error(5, "Input message content type must be InputMessageText"));
  }

  auto r_input_message_text =
      process_input_message_text(DialogId(), std::move(input_message_content), td_->auth_manager_->is_bot());
  if (r_input_message_text.is_error()) {
    return promise.set_error(r_input_message_text.move_as_error());
  }
  InputMessageText input_message_text = r_input_message_text.move_as_ok();

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true);
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Wrong inline message identifier specified"));
  }

  int32 flags = 0;
  if (input_message_text.disable_web_page_preview) {
    flags |= SEND_MESSAGE_FLAG_DISABLE_WEB_PAGE_PREVIEW;
  }
  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(flags, std::move(input_bot_inline_message_id), input_message_text.text.text,
             get_input_message_entities(td_->contacts_manager_.get(), input_message_text.text.entities), nullptr,
             get_input_reply_markup(r_new_reply_markup.ok()));
}

void MessagesManager::edit_inline_message_live_location(const string &inline_message_id,
                                                        tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                        tl_object_ptr<td_api::location> &&input_location,
                                                        Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is available only for bots"));
  }

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true);
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Wrong inline message identifier specified"));
  }

  Location location(input_location);
  if (location.empty() && input_location != nullptr) {
    return promise.set_error(Status::Error(400, "Wrong location specified"));
  }

  int32 flags = 0;
  if (location.empty()) {
    flags |= telegram_api::messages_editMessage::STOP_GEO_LIVE_MASK;
  }
  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(flags, std::move(input_bot_inline_message_id), "", vector<tl_object_ptr<telegram_api::MessageEntity>>(),
             location.empty() ? nullptr : location.get_input_geo_point(),
             get_input_reply_markup(r_new_reply_markup.ok()));
}

void MessagesManager::edit_inline_message_caption(const string &inline_message_id,
                                                  tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                  tl_object_ptr<td_api::formattedText> &&input_caption,
                                                  Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is available only for bots"));
  }

  auto r_caption = process_input_caption(DialogId(), std::move(input_caption), td_->auth_manager_->is_bot());
  if (r_caption.is_error()) {
    return promise.set_error(r_caption.move_as_error());
  }
  auto caption = r_caption.move_as_ok();

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true);
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Wrong inline message identifier specified"));
  }

  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(1 << 11, std::move(input_bot_inline_message_id), caption.text,
             get_input_message_entities(td_->contacts_manager_.get(), caption.entities), nullptr,
             get_input_reply_markup(r_new_reply_markup.ok()));
}

void MessagesManager::edit_inline_message_reply_markup(const string &inline_message_id,
                                                       tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                                       Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is available only for bots"));
  }

  auto r_new_reply_markup = get_reply_markup(std::move(reply_markup), td_->auth_manager_->is_bot(), true, false, true);
  if (r_new_reply_markup.is_error()) {
    return promise.set_error(r_new_reply_markup.move_as_error());
  }

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Wrong inline message identifier specified"));
  }

  td_->create_handler<EditInlineMessageQuery>(std::move(promise))
      ->send(0, std::move(input_bot_inline_message_id), string(), vector<tl_object_ptr<telegram_api::MessageEntity>>(),
             nullptr, get_input_reply_markup(r_new_reply_markup.ok()));
}

int32 MessagesManager::get_message_flags(const Message *m) {
  int32 flags = 0;
  if (m->reply_to_message_id.is_valid()) {
    flags |= SEND_MESSAGE_FLAG_IS_REPLY;
  }
  if (m->disable_web_page_preview) {
    flags |= SEND_MESSAGE_FLAG_DISABLE_WEB_PAGE_PREVIEW;
  }
  if (m->reply_markup != nullptr) {
    flags |= SEND_MESSAGE_FLAG_HAS_REPLY_MARKUP;
  }
  if (m->disable_notification) {
    flags |= SEND_MESSAGE_FLAG_DISABLE_NOTIFICATION;
  }
  if (m->from_background) {
    flags |= SEND_MESSAGE_FLAG_FROM_BACKGROUND;
  }
  if (m->clear_draft) {
    flags |= SEND_MESSAGE_FLAG_CLEAR_DRAFT;
  }
  return flags;
}

bool MessagesManager::can_set_game_score(DialogId dialog_id, const Message *m) const {
  if (m == nullptr) {
    return false;
  }
  if (m->message_id.is_yet_unsent()) {
    return false;
  }
  if (m->message_id.is_local()) {
    return false;
  }
  if (m->via_bot_user_id.is_valid() && !m->is_outgoing) {
    return false;
  }

  if (!td_->auth_manager_->is_bot()) {
    return false;
  }
  if (m->reply_markup == nullptr || m->reply_markup->type != ReplyMarkup::Type::InlineKeyboard ||
      m->reply_markup->inline_keyboard.empty()) {
    return false;
  }

  DialogId my_dialog_id(td_->contacts_manager_->get_my_id("can_set_game_score"));
  switch (dialog_id.get_type()) {
    case DialogType::User:
      if (!m->is_outgoing && dialog_id != my_dialog_id) {
        return false;
      }
      break;
    case DialogType::Chat:
      if (!m->is_outgoing) {
        return false;
      }
      break;
    case DialogType::Channel: {
      if (m->via_bot_user_id.is_valid()) {
        // outgoing via_bot messages can always be edited
        break;
      }
      auto channel_id = dialog_id.get_channel_id();
      auto channel_status = td_->contacts_manager_->get_channel_status(channel_id);
      if (m->is_channel_post) {
        if (!channel_status.can_edit_messages() && (!channel_status.can_post_messages() || !m->is_outgoing)) {
          return false;
        }
      } else {
        if (!m->is_outgoing) {
          return false;
        }
      }
      break;
    }
    case DialogType::SecretChat:
      return false;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }

  return m->content->get_id() == MessageGame::ID;
}

void MessagesManager::set_game_score(FullMessageId full_message_id, bool edit_message, UserId user_id, int32 score,
                                     bool force, Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is available only for bots"));
  }

  LOG(INFO) << "Begin to set game score of " << user_id << " in " << full_message_id;
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return promise.set_error(Status::Error(5, "Chat not found"));
  }

  if (!have_input_peer(dialog_id, AccessRights::Edit)) {
    return promise.set_error(Status::Error(5, "Can't access the chat"));
  }

  auto message_id = full_message_id.get_message_id();
  const Message *message = get_message_force(d, message_id);
  if (message == nullptr) {
    return promise.set_error(Status::Error(5, "Message not found"));
  }

  auto input_user = td_->contacts_manager_->get_input_user(user_id);
  if (input_user == nullptr) {
    return promise.set_error(Status::Error(400, "Wrong user identifier specified"));
  }

  if (!can_set_game_score(dialog_id, message)) {
    return promise.set_error(Status::Error(5, "Game score can't be set"));
  }

  send_closure(td_->create_net_actor<SetGameScoreActor>(std::move(promise)), &SetGameScoreActor::send, dialog_id,
               message_id, edit_message, std::move(input_user), score, force,
               get_sequence_dispatcher_id(dialog_id, -1));
}

void MessagesManager::set_inline_game_score(const string &inline_message_id, bool edit_message, UserId user_id,
                                            int32 score, bool force, Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is available only for bots"));
  }

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Wrong inline message identifier specified"));
  }

  auto input_user = td_->contacts_manager_->get_input_user(user_id);
  if (input_user == nullptr) {
    return promise.set_error(Status::Error(400, "Wrong user identifier specified"));
  }

  td_->create_handler<SetInlineGameScoreQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), edit_message, std::move(input_user), score, force);
}

int64 MessagesManager::get_game_high_scores(FullMessageId full_message_id, UserId user_id, Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    promise.set_error(Status::Error(3, "Method is available only for bots"));
    return 0;
  }

  LOG(INFO) << "Begin to get game high scores of " << user_id << " in " << full_message_id;
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    promise.set_error(Status::Error(5, "Chat not found"));
    return 0;
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    promise.set_error(Status::Error(5, "Can't access the chat"));
    return 0;
  }

  auto message_id = full_message_id.get_message_id();
  const Message *message = get_message_force(d, message_id);
  if (message == nullptr) {
    promise.set_error(Status::Error(5, "Message not found"));
    return 0;
  }
  if (!message_id.is_server()) {
    promise.set_error(Status::Error(5, "Wrong message identifier specified"));
    return 0;
  }

  auto input_user = td_->contacts_manager_->get_input_user(user_id);
  if (input_user == nullptr) {
    promise.set_error(Status::Error(400, "Wrong user identifier specified"));
    return 0;
  }

  int64 random_id = 0;
  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || game_high_scores_.find(random_id) != game_high_scores_.end());
  game_high_scores_[random_id];  // reserve place for result

  td_->create_handler<GetGameHighScoresQuery>(std::move(promise))
      ->send(dialog_id, message_id, std::move(input_user), random_id);
  return random_id;
}

int64 MessagesManager::get_inline_game_high_scores(const string &inline_message_id, UserId user_id,
                                                   Promise<Unit> &&promise) {
  if (!td_->auth_manager_->is_bot()) {
    promise.set_error(Status::Error(3, "Method is available only for bots"));
    return 0;
  }

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    promise.set_error(Status::Error(400, "Wrong inline message identifier specified"));
    return 0;
  }

  auto input_user = td_->contacts_manager_->get_input_user(user_id);
  if (input_user == nullptr) {
    promise.set_error(Status::Error(400, "Wrong user identifier specified"));
    return 0;
  }

  int64 random_id = 0;
  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || game_high_scores_.find(random_id) != game_high_scores_.end());
  game_high_scores_[random_id];  // reserve place for result

  td_->create_handler<GetInlineGameHighScoresQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), std::move(input_user), random_id);
  return random_id;
}

void MessagesManager::on_get_game_high_scores(int64 random_id,
                                              tl_object_ptr<telegram_api::messages_highScores> &&high_scores) {
  auto it = game_high_scores_.find(random_id);
  CHECK(it != game_high_scores_.end());
  auto &result = it->second;
  CHECK(result == nullptr);

  if (high_scores == nullptr) {
    game_high_scores_.erase(it);
    return;
  }

  td_->contacts_manager_->on_get_users(std::move(high_scores->users_));

  result = make_tl_object<td_api::gameHighScores>();

  for (auto &high_score : high_scores->scores_) {
    int32 position = high_score->pos_;
    if (position <= 0) {
      LOG(ERROR) << "Receive wrong position = " << position;
      continue;
    }
    UserId user_id(high_score->user_id_);
    LOG_IF(ERROR, !td_->contacts_manager_->have_user(user_id)) << "Have no info about " << user_id;
    int32 score = high_score->score_;
    if (score < 0) {
      LOG(ERROR) << "Receive wrong score = " << score;
      continue;
    }
    result->scores_.push_back(make_tl_object<td_api::gameHighScore>(
        position, td_->contacts_manager_->get_user_id_object(user_id, "gameHighScore"), score));
  }
}

tl_object_ptr<td_api::gameHighScores> MessagesManager::get_game_high_scores_object(int64 random_id) {
  auto it = game_high_scores_.find(random_id);
  CHECK(it != game_high_scores_.end());
  auto result = std::move(it->second);
  game_high_scores_.erase(it);
  return result;
}

unique_ptr<MessagesManager::MessageForwardInfo> MessagesManager::get_message_forward_info(
    tl_object_ptr<telegram_api::messageFwdHeader> &&forward_header) {
  if (forward_header == nullptr) {
    return nullptr;
  }

  if (forward_header->date_ <= 0) {
    LOG(ERROR) << "Wrong date in message forward header: " << oneline(to_string(forward_header));
    return nullptr;
  }

  auto flags = forward_header->flags_;
  UserId sender_user_id;
  ChannelId channel_id;
  MessageId message_id;
  string author_signature;
  DialogId from_dialog_id;
  MessageId from_message_id;
  if ((flags & MESSAGE_FORWARD_HEADER_FLAG_HAS_AUTHOR_ID) != 0) {
    sender_user_id = UserId(forward_header->from_id_);
    if (!sender_user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid sender id in message forward header: " << oneline(to_string(forward_header));
      sender_user_id = UserId();
    }
  }
  if ((flags & MESSAGE_FORWARD_HEADER_FLAG_HAS_CHANNEL_ID) != 0) {
    channel_id = ChannelId(forward_header->channel_id_);
    if (!channel_id.is_valid()) {
      LOG(ERROR) << "Receive invalid channel id in message forward header: " << oneline(to_string(forward_header));
    }
  }
  if ((flags & MESSAGE_FORWARD_HEADER_FLAG_HAS_MESSAGE_ID) != 0) {
    message_id = MessageId(ServerMessageId(forward_header->channel_post_));
    if (!message_id.is_valid()) {
      LOG(ERROR) << "Receive " << message_id << " in message forward header: " << oneline(to_string(forward_header));
      message_id = MessageId();
    }
  }
  if ((flags & MESSAGE_FORWARD_HEADER_FLAG_HAS_AUTHOR_SIGNATURE) != 0) {
    author_signature = std::move(forward_header->post_author_);
  }
  if ((flags & MESSAGE_FORWARD_HEADER_FLAG_HAS_SAVED_FROM) != 0) {
    from_dialog_id = DialogId(forward_header->saved_from_peer_);
    from_message_id = MessageId(ServerMessageId(forward_header->saved_from_msg_id_));
    if (!from_dialog_id.is_valid() || !from_message_id.is_valid()) {
      LOG(ERROR) << "Receive " << from_message_id << " in " << from_dialog_id
                 << " in message forward header: " << oneline(to_string(forward_header));
      from_dialog_id = DialogId();
      from_message_id = MessageId();
    }
  }

  DialogId dialog_id;
  if (!channel_id.is_valid()) {
    if (sender_user_id.is_valid()) {
      if (message_id.is_valid()) {
        LOG(ERROR) << "Receive non-empty message id in message forward header: " << oneline(to_string(forward_header));
        message_id = MessageId();
      }
    } else {
      LOG(ERROR) << "Receive wrong message forward header: " << oneline(to_string(forward_header));
      return nullptr;
    }
  } else {
    LOG_IF(ERROR, td_->contacts_manager_->have_min_channel(channel_id)) << "Receive forward from min channel";
    dialog_id = DialogId(channel_id);
    force_create_dialog(dialog_id, "message forward info");
    if (sender_user_id.is_valid()) {
      LOG(ERROR) << "Receive valid sender user id in message forward header: " << oneline(to_string(forward_header));
      sender_user_id = UserId();
    }
  }
  if (from_dialog_id.is_valid()) {
    force_create_dialog(from_dialog_id, "message forward from info");
  }

  return make_unique<MessageForwardInfo>(sender_user_id, forward_header->date_, dialog_id, message_id, author_signature,
                                         from_dialog_id, from_message_id);
}

tl_object_ptr<td_api::MessageForwardInfo> MessagesManager::get_message_forward_info_object(
    const unique_ptr<MessageForwardInfo> &forward_info) const {
  if (forward_info == nullptr) {
    return nullptr;
  }

  if (forward_info->dialog_id.is_valid()) {
    return make_tl_object<td_api::messageForwardedPost>(
        forward_info->dialog_id.get(), forward_info->author_signature, forward_info->date,
        forward_info->message_id.get(), forward_info->from_dialog_id.get(), forward_info->from_message_id.get());
  }
  return make_tl_object<td_api::messageForwardedFromUser>(
      td_->contacts_manager_->get_user_id_object(forward_info->sender_user_id, "messageForwardedFromUser"),
      forward_info->date, forward_info->from_dialog_id.get(), forward_info->from_message_id.get());
}

Result<unique_ptr<ReplyMarkup>> MessagesManager::get_dialog_reply_markup(
    DialogId dialog_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup_ptr) const {
  if (reply_markup_ptr == nullptr) {
    return nullptr;
  }

  auto dialog_type = dialog_id.get_type();
  bool is_broadcast = is_broadcast_channel(dialog_id);

  bool only_inline_keyboard = is_broadcast;
  bool request_buttons_allowed = dialog_type == DialogType::User;
  bool switch_inline_current_chat_buttons_allowed = !is_broadcast;

  TRY_RESULT(reply_markup,
             get_reply_markup(std::move(reply_markup_ptr), td_->auth_manager_->is_bot(), only_inline_keyboard,
                              request_buttons_allowed, switch_inline_current_chat_buttons_allowed));
  if (reply_markup == nullptr) {
    return nullptr;
  }

  switch (dialog_type) {
    case DialogType::User:
      if (reply_markup->type != ReplyMarkup::Type::InlineKeyboard) {
        reply_markup->is_personal = false;
      }
      break;
    case DialogType::Channel:
    case DialogType::Chat:
    case DialogType::SecretChat:
    case DialogType::None:
      // nothing special
      break;
    default:
      UNREACHABLE();
  }

  return std::move(reply_markup);
}

class MessagesManager::ForwardMessagesLogEvent {
 public:
  DialogId to_dialog_id;
  DialogId from_dialog_id;
  vector<MessageId> message_ids;
  vector<Message *> messages_in;
  vector<unique_ptr<Message>> messages_out;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(to_dialog_id, storer);
    td::store(from_dialog_id, storer);
    td::store(message_ids, storer);

    td::store(narrow_cast<int32>(messages_in.size()), storer);
    for (auto m : messages_in) {
      td::store(*m, storer);
    }
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(to_dialog_id, parser);
    td::parse(from_dialog_id, parser);
    td::parse(message_ids, parser);

    CHECK(messages_out.empty());
    int32 size = parser.fetch_int();
    messages_out.resize(size);
    for (auto &m_out : messages_out) {
      m_out = make_unique<Message>();
      td::parse(*m_out, parser);
    }
  }
};

void MessagesManager::do_forward_messages(DialogId to_dialog_id, DialogId from_dialog_id,
                                          const vector<Message *> &messages, const vector<MessageId> &message_ids,
                                          int64 logevent_id) {
  CHECK(messages.size() == message_ids.size());
  if (messages.empty()) {
    return;
  }

  if (logevent_id == 0 && G()->parameters().use_message_db) {
    auto logevent = ForwardMessagesLogEvent{to_dialog_id, from_dialog_id, message_ids, messages, Auto()};
    auto storer = LogEventStorerImpl<ForwardMessagesLogEvent>(logevent);
    logevent_id = BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::ForwardMessages, storer);
  }

  Promise<> promise;
  if (logevent_id != 0) {
    promise = PromiseCreator::lambda([logevent_id](Result<Unit> result) mutable {
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }
    });
  }

  int32 flags = 0;
  if (messages[0]->disable_notification) {
    flags |= SEND_MESSAGE_FLAG_DISABLE_NOTIFICATION;
  }
  if (messages[0]->from_background) {
    flags |= SEND_MESSAGE_FLAG_FROM_BACKGROUND;
  }
  if (messages[0]->media_album_id != 0) {
    flags |= SEND_MESSAGE_FLAG_GROUP_MEDIA;
  }
  if (messages[0]->in_game_share) {
    flags |= SEND_MESSAGE_FLAG_WITH_MY_SCORE;
  }

  vector<int64> random_ids =
      transform(messages, [this, to_dialog_id](const Message *m) { return begin_send_message(to_dialog_id, m); });
  send_closure(td_->create_net_actor<ForwardMessagesActor>(std::move(promise)), &ForwardMessagesActor::send, flags,
               to_dialog_id, from_dialog_id, message_ids, std::move(random_ids),
               get_sequence_dispatcher_id(to_dialog_id, -1));
}

Result<MessageId> MessagesManager::forward_message(DialogId to_dialog_id, DialogId from_dialog_id, MessageId message_id,
                                                   bool disable_notification, bool from_background,
                                                   bool in_game_share) {
  TRY_RESULT(result, forward_messages(to_dialog_id, from_dialog_id, {message_id}, disable_notification, from_background,
                                      in_game_share, false));
  CHECK(result.size() == 1);
  auto sent_message_id = result[0];
  if (sent_message_id == MessageId()) {
    return Status::Error(11, "Message can't be forwarded");
  }
  return sent_message_id;
}

Result<vector<MessageId>> MessagesManager::forward_messages(DialogId to_dialog_id, DialogId from_dialog_id,
                                                            vector<MessageId> message_ids, bool disable_notification,
                                                            bool from_background, bool in_game_share, bool as_album) {
  if (message_ids.size() > 100) {  // TODO replace with const from config or implement mass-forward
    return Status::Error(4, "Too much messages to forward");
  }
  if (message_ids.empty()) {
    return Status::Error(4, "There is no messages to forward");
  }

  Dialog *from_dialog = get_dialog_force(from_dialog_id);
  if (from_dialog == nullptr) {
    return Status::Error(5, "Chat to forward messages from not found");
  }
  if (!have_input_peer(from_dialog_id, AccessRights::Read)) {
    return Status::Error(5, "Can't access the chat to forward messages from");
  }
  if (from_dialog_id.get_type() == DialogType::SecretChat) {
    return Status::Error(7, "Can't forward messages from secret chats");
  }

  Dialog *to_dialog = get_dialog_force(to_dialog_id);
  if (to_dialog == nullptr) {
    return Status::Error(5, "Chat to forward messages to not found");
  }

  TRY_STATUS(can_send_message(to_dialog_id));

  for (auto message_id : message_ids) {
    if (!message_id.is_valid()) {
      return Status::Error(5, "Invalid message identifier");
    }
  }

  int64 media_album_id = 0;
  if (as_album && message_ids.size() > 1 && message_ids.size() <= MAX_GROUPED_MESSAGES) {
    do {
      media_album_id = Random::secure_int64();
    } while (media_album_id >= 0 || pending_message_group_sends_.count(media_album_id) != 0);
  }

  bool to_secret = to_dialog_id.get_type() == DialogType::SecretChat;

  vector<MessageId> result(message_ids.size());
  vector<Message *> forwarded_messages;
  vector<MessageId> forwarded_message_ids;
  vector<unique_ptr<MessageContent>> unforwarded_message_contents(message_ids.size());
  vector<bool> unforwarded_message_disable_web_page_previews(message_ids.size());
  auto my_id = td_->contacts_manager_->get_my_id("forward_message");
  bool need_update_dialog_pos = false;
  for (size_t i = 0; i < message_ids.size(); i++) {
    MessageId message_id = get_persistent_message_id(from_dialog, message_ids[i]);

    const Message *forwarded_message = get_message_force(from_dialog, message_id);
    if (forwarded_message == nullptr) {
      LOG(INFO) << "Can't find " << message_id << " to forward";
      continue;
    }

    if (!can_forward_message(from_dialog_id, forwarded_message)) {
      LOG(INFO) << "Can't forward " << message_id;
      continue;
    }

    unique_ptr<MessageContent> content = dup_message_content(to_dialog_id, forwarded_message->content.get(), true);
    if (content == nullptr) {
      LOG(INFO) << "Can't forward " << message_id;
      continue;
    }

    auto can_send_status = can_send_message_content(to_dialog_id, content.get(), true);
    if (can_send_status.is_error()) {
      LOG(INFO) << "Can't forward " << message_id << ": " << can_send_status.message();
      continue;
    }

    if (!message_id.is_server() || to_secret) {
      unforwarded_message_contents[i] = std::move(content);
      unforwarded_message_disable_web_page_previews[i] = forwarded_message->disable_web_page_preview;
      continue;
    }

    auto content_id = content->get_id();
    if (media_album_id != 0 && !is_allowed_media_group_content(content_id)) {
      media_album_id = 0;
      for (auto m : forwarded_messages) {
        m->media_album_id = 0;
      }
    }

    bool is_game = content_id == MessageGame::ID;
    unique_ptr<MessageForwardInfo> forward_info;
    if (!is_game && content_id != MessageAudio::ID) {
      DialogId saved_from_dialog_id;
      MessageId saved_from_message_id;
      if (to_dialog_id == DialogId(my_id)) {
        saved_from_dialog_id = from_dialog_id;
        saved_from_message_id = message_id;
      }

      if (forwarded_message->forward_info != nullptr) {
        forward_info = make_unique<MessageForwardInfo>(*forwarded_message->forward_info);
        forward_info->from_dialog_id = saved_from_dialog_id;
        forward_info->from_message_id = saved_from_message_id;
      } else {
        if (from_dialog_id != DialogId(my_id)) {
          if (!forwarded_message->is_channel_post) {
            forward_info =
                make_unique<MessageForwardInfo>(forwarded_message->sender_user_id, forwarded_message->date, DialogId(),
                                                MessageId(), "", saved_from_dialog_id, saved_from_message_id);
          } else {
            CHECK(from_dialog_id.get_type() == DialogType::Channel);
            MessageId forwarded_message_id = forwarded_message->message_id;
            auto author_signature = forwarded_message->sender_user_id.is_valid()
                                        ? td_->contacts_manager_->get_user_title(forwarded_message->sender_user_id)
                                        : forwarded_message->author_signature;
            forward_info = make_unique<MessageForwardInfo>(UserId(), forwarded_message->date, from_dialog_id,
                                                           forwarded_message_id, std::move(author_signature),
                                                           saved_from_dialog_id, saved_from_message_id);
          }
        }
      }
    }

    Message *m = get_message_to_send(to_dialog, MessageId(), disable_notification, from_background, std::move(content),
                                     &need_update_dialog_pos, std::move(forward_info));
    m->debug_forward_from = from_dialog_id;
    m->via_bot_user_id = forwarded_message->via_bot_user_id;
    m->media_album_id = media_album_id;
    m->in_game_share = in_game_share;
    if (forwarded_message->views > 0) {
      m->views = forwarded_message->views;
    }

    if (is_game) {
      if (m->via_bot_user_id == my_id) {
        m->via_bot_user_id = UserId();
      } else if (m->via_bot_user_id == UserId()) {
        m->via_bot_user_id = forwarded_message->sender_user_id;
      }
    }

    result[i] = m->message_id;
    forwarded_messages.push_back(m);
    forwarded_message_ids.push_back(message_id);

    send_update_new_message(to_dialog, m, true);
  }

  if (!forwarded_messages.empty()) {
    do_forward_messages(to_dialog_id, from_dialog_id, forwarded_messages, forwarded_message_ids, 0);
  }

  for (size_t i = 0; i < unforwarded_message_contents.size(); i++) {
    if (unforwarded_message_contents[i] != nullptr) {
      Message *m = get_message_to_send(to_dialog, MessageId(), disable_notification, from_background,
                                       std::move(unforwarded_message_contents[i]), &need_update_dialog_pos);
      m->disable_web_page_preview = unforwarded_message_disable_web_page_previews[i];
      if (to_secret) {
        m->media_album_id = media_album_id;
      }

      save_send_message_logevent(to_dialog_id, m);
      do_send_message(to_dialog_id, m);
      result[i] = m->message_id;

      send_update_new_message(to_dialog, m, true);
    }
  }

  if (need_update_dialog_pos) {
    send_update_chat_last_message(to_dialog, "forward_messages");
  }

  return result;
}

Result<MessageId> MessagesManager::send_dialog_set_ttl_message(DialogId dialog_id, int32 ttl) {
  if (dialog_id.get_type() != DialogType::SecretChat) {
    return Status::Error(5, "Can't set chat ttl in non-secret chat");
  }

  if (ttl < 0) {
    return Status::Error(5, "Message ttl can't be negative");
  }

  LOG(INFO) << "Begin to set ttl in " << dialog_id << " to " << ttl;

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(5, "Chat not found");
  }

  TRY_STATUS(can_send_message(dialog_id));
  bool need_update_dialog_pos = false;
  Message *m =
      get_message_to_send(d, MessageId(), false, false, make_unique<MessageChatSetTtl>(ttl), &need_update_dialog_pos);

  send_update_new_message(d, m, true);
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "send_dialog_set_ttl_message");
  }

  int64 random_id = begin_send_message(dialog_id, m);

  send_closure(td_->secret_chats_manager_, &SecretChatsManager::send_set_ttl_message, dialog_id.get_secret_chat_id(),
               ttl, random_id, Promise<>());  // TODO Promise

  return m->message_id;
}

Status MessagesManager::send_screenshot_taken_notification_message(DialogId dialog_id) {
  auto dialog_type = dialog_id.get_type();
  if (dialog_type != DialogType::User && dialog_type != DialogType::SecretChat) {
    return Status::Error(5, "Notification about taken screenshot can be sent only in private and secret chats");
  }

  LOG(INFO) << "Begin to send notification about taken screenshot in " << dialog_id;

  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    return Status::Error(5, "Chat not found");
  }

  TRY_STATUS(can_send_message(dialog_id));

  if (dialog_type == DialogType::User) {
    bool need_update_dialog_pos = false;
    const Message *m = get_message_to_send(d, MessageId(), false, false, make_unique<MessageScreenshotTaken>(),
                                           &need_update_dialog_pos);

    do_send_screenshot_taken_notification_message(dialog_id, m, 0);

    send_update_new_message(d, m, true);
    if (need_update_dialog_pos) {
      send_update_chat_last_message(d, "send_screenshot_taken_notification_message");
    }
  } else {
    send_closure(td_->secret_chats_manager_, &SecretChatsManager::notify_screenshot_taken,
                 dialog_id.get_secret_chat_id(),
                 Promise<>());  // TODO Promise
  }

  return Status::OK();
}

class MessagesManager::SendScreenshotTakenNotificationMessageLogEvent {
 public:
  DialogId dialog_id;
  const Message *m_in = nullptr;
  unique_ptr<Message> m_out;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id, storer);
    td::store(*m_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id, parser);
    CHECK(m_out == nullptr);
    m_out = make_unique<Message>();
    td::parse(*m_out, parser);
  }
};

uint64 MessagesManager::save_send_screenshot_taken_notification_message_logevent(DialogId dialog_id, const Message *m) {
  if (!G()->parameters().use_message_db) {
    return 0;
  }

  CHECK(m != nullptr);
  LOG(INFO) << "Save " << FullMessageId(dialog_id, m->message_id) << " to binlog";
  SendScreenshotTakenNotificationMessageLogEvent logevent;
  logevent.dialog_id = dialog_id;
  logevent.m_in = m;
  auto storer = LogEventStorerImpl<SendScreenshotTakenNotificationMessageLogEvent>(logevent);
  return BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::SendScreenshotTakenNotificationMessage,
                           storer);
}

void MessagesManager::do_send_screenshot_taken_notification_message(DialogId dialog_id, const Message *m,
                                                                    uint64 logevent_id) {
  LOG(INFO) << "Do send screenshot taken notification " << FullMessageId(dialog_id, m->message_id);
  CHECK(dialog_id.get_type() == DialogType::User);

  if (logevent_id == 0) {
    logevent_id = save_send_screenshot_taken_notification_message_logevent(dialog_id, m);
  }

  Promise<> promise;
  if (logevent_id != 0) {
    promise = PromiseCreator::lambda([logevent_id](Result<Unit> result) mutable {
      LOG(INFO) << "Erase logevent_id " << logevent_id;
      if (!G()->close_flag()) {
        BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
      }
    });
  }

  int64 random_id = begin_send_message(dialog_id, m);
  td_->create_handler<SendScreenshotNotificationQuery>(std::move(promise))->send(dialog_id, random_id);
}

bool MessagesManager::on_update_message_id(int64 random_id, MessageId new_message_id, const string &source) {
  if (!new_message_id.is_valid()) {
    LOG(ERROR) << "Receive " << new_message_id << " in update message id with random_id " << random_id << " from "
               << source;
    auto it = debug_being_sent_messages_.find(random_id);
    if (it == debug_being_sent_messages_.end()) {
      LOG(ERROR) << "Message with random_id " << random_id << " was not sent";
      return false;
    }
    auto dialog_id = it->second;
    if (!dialog_id.is_valid()) {
      LOG(ERROR) << "Sent message is in invalid " << dialog_id;
      return false;
    }
    if (!have_dialog(dialog_id)) {
      LOG(ERROR) << "Sent message is in not found " << dialog_id;
      return false;
    }
    LOG(ERROR) << "Receive " << new_message_id << " in update message id with random_id " << random_id << " in "
               << dialog_id;
    return false;
  }

  auto it = being_sent_messages_.find(random_id);
  if (it == being_sent_messages_.end()) {
    // update about new message sent from other device or service message
    LOG(INFO) << "Receive not send outgoing " << new_message_id << " with random_id = " << random_id;
    return true;
  }

  auto dialog_id = it->second.get_dialog_id();
  auto old_message_id = it->second.get_message_id();

  being_sent_messages_.erase(it);

  update_message_ids_[FullMessageId(dialog_id, new_message_id)] = old_message_id;
  return true;
}

bool MessagesManager::on_get_dialog_error(DialogId dialog_id, const Status &status, const string &source) {
  if (status.message() == CSlice("SESSION_REVOKED") || status.message() == CSlice("USER_DEACTIVATED")) {
    // authorization is lost
    return true;
  }
  if (status.code() == 420 || status.code() == 429) {
    // flood wait
    return true;
  }
  if (status.message() == CSlice("BOT_METHOD_INVALID")) {
    LOG(ERROR) << "Receive BOT_METHOD_INVALID from " << source;
    return true;
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::SecretChat:
      // to be implemented if necessary
      break;
    case DialogType::Channel:
      return td_->contacts_manager_->on_get_channel_error(dialog_id.get_channel_id(), status, source);
    case DialogType::None:
      // to be implemented if necessary
      break;
    default:
      UNREACHABLE();
  }
  return false;
}

void MessagesManager::on_dialog_updated(DialogId dialog_id, const char *source) {
  if (G()->parameters().use_message_db) {
    LOG(INFO) << "Update " << dialog_id << " from " << source;
    pending_updated_dialog_timeout_.add_timeout_in(dialog_id.get(), MAX_SAVE_DIALOG_DELAY);
  }
}

void MessagesManager::send_update_new_message(Dialog *d, const Message *m, bool force) {
  CHECK(d != nullptr);
  CHECK(m != nullptr);

  DialogId my_dialog_id(td_->contacts_manager_->get_my_id("send_update_new_message"));
  bool disable_notification =
      m->disable_notification || m->is_outgoing || d->dialog_id == my_dialog_id || td_->auth_manager_->is_bot();
  if (m->message_id.get() <= d->last_read_inbox_message_id.get()) {
    LOG(INFO) << "Disable notification for read " << m->message_id << " in " << d->dialog_id;
    disable_notification = true;
  }
  if (!disable_notification && d->dialog_id.get_type() == DialogType::Channel) {
    if (!td_->contacts_manager_->get_channel_status(d->dialog_id.get_channel_id()).is_member()) {
      disable_notification = true;
    }
  }
  bool have_settings = true;
  DialogId settings_dialog_id;
  if (!disable_notification) {
    Dialog *settings_dialog;
    if (!m->contains_mention || !m->sender_user_id.is_valid()) {
      // use notification settings from the dialog
      settings_dialog_id = d->dialog_id;
      settings_dialog = d;
    } else {
      // have a mention, so use notification settings from the dialog with the sender
      settings_dialog_id = DialogId(m->sender_user_id);
      settings_dialog = get_dialog_force(settings_dialog_id);
    }

    auto notification_settings = get_dialog_notification_settings(settings_dialog, settings_dialog_id);
    if (notification_settings == nullptr ||  // unknown megagroup without mention
        notification_settings->mute_until > G()->unix_time()) {
      disable_notification = true;
    }
    if (settings_dialog == nullptr || !settings_dialog->notification_settings.is_synchronized) {
      have_settings = false;
    }
  }

  if (!force && (!have_settings || !d->pending_update_new_messages.empty())) {
    LOG(INFO) << "Delay update new message for " << m->message_id << " in " << d->dialog_id;
    if (d->pending_update_new_messages.empty()) {
      create_actor<SleepActor>(
          "FlushPendingUpdateNewMessagesSleepActor", 5.0,
          PromiseCreator::lambda([actor_id = actor_id(this), dialog_id = d->dialog_id](Result<Unit> result) {
            send_closure(actor_id, &MessagesManager::flush_pending_update_new_messages, dialog_id);
          }))
          .release();
    }
    d->pending_update_new_messages.push_back(m->message_id);
    auto promise = PromiseCreator::lambda([actor_id = actor_id(this), dialog_id = d->dialog_id](Result<Unit> result) {
      send_closure(actor_id, &MessagesManager::flush_pending_update_new_messages, dialog_id);
    });
    send_get_dialog_query(settings_dialog_id, std::move(promise));  // TODO use GetNotifySettingsQuery when possible
    return;
  }

  LOG_IF(WARNING, !have_settings) << "Have no notification settings for " << settings_dialog_id;
  send_closure(G()->td(), &Td::send_update,
               make_tl_object<td_api::updateNewMessage>(get_message_object(d->dialog_id, m), disable_notification,
                                                        m->contains_mention));
}

void MessagesManager::flush_pending_update_new_messages(DialogId dialog_id) {
  auto d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  if (d->pending_update_new_messages.empty()) {
    return;
  }
  auto message_ids = std::move(d->pending_update_new_messages);
  reset_to_empty(d->pending_update_new_messages);
  for (auto message_id : message_ids) {
    auto m = get_message(d, message_id);
    if (m != nullptr) {
      send_update_new_message(d, m, true);
    }
  }
}

void MessagesManager::send_update_message_send_succeeded(Dialog *d, MessageId old_message_id, const Message *m) const {
  CHECK(m != nullptr);
  d->yet_unsent_message_id_to_persistent_message_id.emplace(old_message_id, m->message_id);
  send_closure(
      G()->td(), &Td::send_update,
      make_tl_object<td_api::updateMessageSendSucceeded>(get_message_object(d->dialog_id, m), old_message_id.get()));
}

void MessagesManager::send_update_message_content(DialogId dialog_id, MessageId message_id,
                                                  const MessageContent *content, int32 message_date,
                                                  bool is_content_secret, const char *source) const {
  LOG(INFO) << "Send updateMessageContent for " << message_id << " in " << dialog_id << " from " << source;
  CHECK(have_dialog(dialog_id)) << "Send updateMessageContent in unknown " << dialog_id << " from " << source
                                << " with load count " << loaded_dialogs_.count(dialog_id);
  send_closure(
      G()->td(), &Td::send_update,
      make_tl_object<td_api::updateMessageContent>(
          dialog_id.get(), message_id.get(), get_message_content_object(content, message_date, is_content_secret)));
}

void MessagesManager::send_update_message_edited(FullMessageId full_message_id) {
  return send_update_message_edited(full_message_id.get_dialog_id(), get_message(full_message_id));
}

void MessagesManager::send_update_message_edited(DialogId dialog_id, const Message *m) {
  CHECK(m != nullptr);
  cancel_user_dialog_action(dialog_id, m);
  send_closure(G()->td(), &Td::send_update,
               make_tl_object<td_api::updateMessageEdited>(dialog_id.get(), m->message_id.get(), m->edit_date,
                                                           get_reply_markup_object(m->reply_markup)));
}

void MessagesManager::send_update_delete_messages(DialogId dialog_id, vector<int64> &&message_ids, bool is_permanent,
                                                  bool from_cache) const {
  if (!message_ids.empty()) {
    CHECK(have_dialog(dialog_id)) << "Wrong " << dialog_id << " in send_update_delete_messages";
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateDeleteMessages>(dialog_id.get(), std::move(message_ids), is_permanent,
                                                              from_cache));
  }
}

void MessagesManager::send_update_chat(Dialog *d) {
  CHECK(d != nullptr);
  CHECK(d->messages == nullptr);
  send_closure(G()->td(), &Td::send_update, make_tl_object<td_api::updateNewChat>(get_chat_object(d)));
}

void MessagesManager::send_update_chat_draft_message(const Dialog *d) {
  CHECK(d != nullptr);
  CHECK(d == get_dialog(d->dialog_id)) << "Wrong " << d->dialog_id << " in send_update_chat_draft_message";
  on_dialog_updated(d->dialog_id, "send_update_chat_draft_message");
  send_closure(G()->td(), &Td::send_update,
               make_tl_object<td_api::updateChatDraftMessage>(
                   d->dialog_id.get(), get_draft_message_object(d->draft_message),
                   DialogDate(d->order, d->dialog_id) <= last_dialog_date_ ? d->order : 0));
}

void MessagesManager::send_update_chat_last_message(Dialog *d, const char *source) {
  update_dialog_pos(d, false, source, false);
  send_update_chat_last_message_impl(d, source);
}

void MessagesManager::send_update_chat_last_message_impl(const Dialog *d, const char *source) const {
  CHECK(d != nullptr);
  CHECK(d == get_dialog(d->dialog_id)) << "Wrong " << d->dialog_id << " in send_update_chat_last_message from "
                                       << source;
  LOG(INFO) << "Send updateChatLastMessage in " << d->dialog_id << " to " << d->last_message_id << " from " << source;
  auto update = make_tl_object<td_api::updateChatLastMessage>(
      d->dialog_id.get(), get_message_object(d->dialog_id, get_message(d, d->last_message_id)),
      DialogDate(d->order, d->dialog_id) <= last_dialog_date_ ? d->order : 0);
  send_closure(G()->td(), &Td::send_update, std::move(update));
}

void MessagesManager::send_update_unread_message_count(DialogId dialog_id, bool force, const char *source) {
  if (!td_->auth_manager_->is_bot() && G()->parameters().use_message_db) {
    CHECK(is_unread_count_inited_);
    if (unread_message_total_count_ < 0 || unread_message_muted_count_ < 0 ||
        unread_message_muted_count_ > unread_message_total_count_) {
      LOG(ERROR) << "Unread messafe count became invalid: " << unread_message_total_count_ << '/'
                 << unread_message_total_count_ - unread_message_muted_count_ << " from " << source << " and "
                 << dialog_id;
      if (unread_message_total_count_ < 0) {
        unread_message_total_count_ = 0;
      }
      if (unread_message_muted_count_ < 0) {
        unread_message_muted_count_ = 0;
      }
      if (unread_message_muted_count_ > unread_message_total_count_) {
        unread_message_muted_count_ = unread_message_total_count_;
      }
    }
    G()->td_db()->get_binlog_pmc()->set("unread_message_count",
                                        PSTRING() << unread_message_total_count_ << ' ' << unread_message_muted_count_);
    int32 unread_unmuted_count = unread_message_total_count_ - unread_message_muted_count_;
    if (!force && running_get_difference_) {
      LOG(INFO) << "Postpone updateUnreadMessageCount to " << unread_message_total_count_ << '/' << unread_unmuted_count
                << " from " << source << " and " << dialog_id;
      have_postponed_unread_message_count_update_ = true;
    } else {
      have_postponed_unread_message_count_update_ = false;
      LOG(INFO) << "Send updateUnreadMessageCount to " << unread_message_total_count_ << '/' << unread_unmuted_count
                << " from " << source << " and " << dialog_id;
      send_closure(G()->td(), &Td::send_update,
                   make_tl_object<td_api::updateUnreadMessageCount>(unread_message_total_count_, unread_unmuted_count));
    }
  }
}

void MessagesManager::send_update_chat_read_inbox(const Dialog *d, bool force, const char *source) {
  CHECK(d != nullptr);
  if (!td_->auth_manager_->is_bot()) {
    CHECK(d == get_dialog(d->dialog_id)) << "Wrong " << d->dialog_id << " in send_update_chat_read_inbox from "
                                         << source;
    on_dialog_updated(d->dialog_id, source);
    if (!force && (running_get_difference_ || running_get_channel_difference(d->dialog_id))) {
      LOG(INFO) << "Postpone updateChatReadInbox in " << d->dialog_id << "(" << get_dialog_title(d->dialog_id)
                << ") to " << d->server_unread_count << " + " << d->local_unread_count << " from " << source;
      postponed_chat_read_inbox_updates_.insert(d->dialog_id);
    } else {
      postponed_chat_read_inbox_updates_.erase(d->dialog_id);
      LOG(INFO) << "Send updateChatReadInbox in " << d->dialog_id << "(" << get_dialog_title(d->dialog_id) << ") to "
                << d->server_unread_count << " + " << d->local_unread_count << " from " << source;
      send_closure(G()->td(), &Td::send_update,
                   make_tl_object<td_api::updateChatReadInbox>(d->dialog_id.get(), d->last_read_inbox_message_id.get(),
                                                               d->server_unread_count + d->local_unread_count));
    }
  }
}

void MessagesManager::send_update_chat_read_outbox(const Dialog *d) {
  CHECK(d != nullptr);
  if (!td_->auth_manager_->is_bot()) {
    CHECK(d == get_dialog(d->dialog_id)) << "Wrong " << d->dialog_id << " in send_update_chat_read_outbox";
    on_dialog_updated(d->dialog_id, "send_update_chat_read_outbox");
    send_closure(
        G()->td(), &Td::send_update,
        make_tl_object<td_api::updateChatReadOutbox>(d->dialog_id.get(), d->last_read_outbox_message_id.get()));
  }
}

void MessagesManager::send_update_chat_unread_mention_count(const Dialog *d) {
  CHECK(d != nullptr);
  if (!td_->auth_manager_->is_bot()) {
    CHECK(d == get_dialog(d->dialog_id)) << "Wrong " << d->dialog_id << " in send_update_chat_unread_mention_count";
    LOG(INFO) << "Update unread mention message count in " << d->dialog_id << " to " << d->unread_mention_count;
    on_dialog_updated(d->dialog_id, "send_update_chat_unread_mention_count");
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateChatUnreadMentionCount>(d->dialog_id.get(), d->unread_mention_count));
  }
}

void MessagesManager::on_send_message_get_quick_ack(int64 random_id) {
  auto it = being_sent_messages_.find(random_id);
  if (it == being_sent_messages_.end()) {
    LOG(ERROR) << "Receive quick ack about unknown message with random_id = " << random_id;
    return;
  }

  auto dialog_id = it->second.get_dialog_id();
  auto message_id = it->second.get_message_id();

  send_closure(G()->td(), &Td::send_update,
               make_tl_object<td_api::updateMessageSendAcknowledged>(dialog_id.get(), message_id.get()));
}

void MessagesManager::check_send_message_result(int64 random_id, DialogId dialog_id,
                                                const telegram_api::Updates *updates_ptr, const char *source) {
  CHECK(updates_ptr != nullptr);
  CHECK(source != nullptr);
  auto sent_messages = td_->updates_manager_->get_new_messages(updates_ptr);
  auto sent_messages_random_ids = td_->updates_manager_->get_sent_messages_random_ids(updates_ptr);
  if (sent_messages.size() != 1u || sent_messages_random_ids.size() != 1u ||
      *sent_messages_random_ids.begin() != random_id || get_message_dialog_id(*sent_messages[0]) != dialog_id) {
    LOG(ERROR) << "Receive wrong result for sending message with random_id " << random_id << " from " << source
               << " to " << dialog_id << ": " << oneline(to_string(*updates_ptr));
    if (dialog_id.get_type() == DialogType::Channel) {
      Dialog *d = get_dialog(dialog_id);
      CHECK(d != nullptr);
      get_channel_difference(dialog_id, d->pts, true, "check_send_message_result");
    } else {
      td_->updates_manager_->schedule_get_difference("check_send_message_result");
    }
  }
}

FullMessageId MessagesManager::on_send_message_success(int64 random_id, MessageId new_message_id, int32 date,
                                                       FileId new_file_id, const char *source) {
  CHECK(source != nullptr);
  // do not try to run getDifference from this function
  if (DROP_UPDATES) {
    return {};
  }
  if (!new_message_id.is_valid()) {
    LOG(ERROR) << "Receive " << new_message_id << " as sent message from " << source;
    on_send_message_fail(random_id,
                         Status::Error(500, "Internal server error: receive invalid message id as sent message id"));
    return {};
  }
  if (new_message_id.is_yet_unsent()) {
    LOG(ERROR) << "Receive " << new_message_id << " as sent message from " << source;
    on_send_message_fail(random_id,
                         Status::Error(500, "Internal server error: receive yet unsent message as sent message"));
    return {};
  }

  auto it = being_sent_messages_.find(random_id);
  if (it == being_sent_messages_.end()) {
    LOG(ERROR) << "Result from sendMessage for " << new_message_id << " with random_id " << random_id << " sent at "
               << date << " comes from " << source << " after updateNewMessageId, but was not discarded by pts. "
               << td_->updates_manager_->get_state();
    if (debug_being_sent_messages_.count(random_id) == 0) {
      LOG(ERROR) << "Message with random_id " << random_id << " was mot sent";
      return {};
    }
    auto dialog_id = debug_being_sent_messages_[random_id];
    if (!dialog_id.is_valid()) {
      LOG(ERROR) << "Sent message is in invalid " << dialog_id;
      return {};
    }
    auto d = get_dialog(dialog_id);
    if (d == nullptr) {
      LOG(ERROR) << "Sent message is in not found " << dialog_id;
      return {};
    }
    dump_debug_message_op(d, 7);
    auto m = get_message_force(d, new_message_id);
    if (m == nullptr) {
      LOG(ERROR) << new_message_id << " in " << dialog_id << " not found";
      return {};
    }
    LOG(ERROR) << "Result from sent " << (m->is_outgoing ? "outgoing" : "incoming")
               << (m->forward_info == nullptr ? " not" : "") << " forwarded " << new_message_id
               << " with content of the type " << m->content->get_id() << " in " << dialog_id
               << " comes after updateNewMessageId, current last new is " << d->last_new_message_id << ", last is "
               << d->last_message_id << ". " << td_->updates_manager_->get_state();
    return {};
  }

  auto dialog_id = it->second.get_dialog_id();
  auto old_message_id = it->second.get_message_id();

  being_sent_messages_.erase(it);

  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  bool need_update_dialog_pos = false;
  unique_ptr<Message> sent_message = delete_message(d, old_message_id, false, &need_update_dialog_pos, source);
  if (sent_message == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel
    // don't need to send update to the user, because the message has already been deleted
    LOG(INFO) << "Delete already deleted sent " << new_message_id << " from server";
    delete_messages_from_server(dialog_id, {new_message_id}, true, 0, Auto());
    return {};
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // LOG(ERROR) << "Found " << old_message_id << " in inaccessible " << dialog_id;
    // dump_debug_message_op(d, 5);
  }

  // imitation of update_message(d, sent_message, std::move(new_message), true, &need_update_dialog_pos);
  if (date <= 0) {
    LOG(ERROR) << "Receive " << new_message_id << " in " << dialog_id << " with wrong date " << date;
  } else {
    CHECK(sent_message->date > 0);
    sent_message->date = date;
    CHECK(d->last_message_id != old_message_id);
  }

  // reply_to message may be already deleted
  // but can't use get_message for check, because the message can be already unloaded from the memory
  // if (get_message_force(d, sent_message->reply_to_message_id) == nullptr) {
  //   sent_message->reply_to_message_id = MessageId();
  // }

  bool is_content_changed = false;
  if (new_file_id.is_valid()) {
    int32 content_type = sent_message->content->get_id();
    switch (content_type) {
      case MessageAnimation::ID: {
        auto content = static_cast<MessageAnimation *>(sent_message->content.get());
        if (new_file_id != content->file_id) {
          td_->animations_manager_->merge_animations(new_file_id, content->file_id, false);
          content->file_id = new_file_id;
          is_content_changed = true;
        }
        break;
      }
      case MessageAudio::ID: {
        auto content = static_cast<MessageAudio *>(sent_message->content.get());
        if (new_file_id != content->file_id) {
          td_->audios_manager_->merge_audios(new_file_id, content->file_id, false);
          content->file_id = new_file_id;
          is_content_changed = true;
        }
        break;
      }
      case MessageDocument::ID: {
        auto content = static_cast<MessageDocument *>(sent_message->content.get());
        if (new_file_id != content->file_id) {
          td_->documents_manager_->merge_documents(new_file_id, content->file_id, false);
          content->file_id = new_file_id;
          is_content_changed = true;
        }
        break;
      }
      case MessagePhoto::ID: {
        auto content = static_cast<MessagePhoto *>(sent_message->content.get());
        Photo *photo = &content->photo;
        if (!photo->photos.empty() && photo->photos.back().type == 'i') {
          FileId &old_file_id = photo->photos.back().file_id;
          if (old_file_id != new_file_id) {
            LOG_STATUS(td_->file_manager_->merge(new_file_id, old_file_id));
            old_file_id = new_file_id;
            is_content_changed = true;
          }
        }
        break;
      }
      case MessageSticker::ID: {
        auto content = static_cast<MessageSticker *>(sent_message->content.get());
        if (new_file_id != content->file_id) {
          td_->stickers_manager_->merge_stickers(new_file_id, content->file_id, false);
          content->file_id = new_file_id;
          is_content_changed = true;
        }
        break;
      }
      case MessageVideo::ID: {
        auto content = static_cast<MessageVideo *>(sent_message->content.get());
        if (new_file_id != content->file_id) {
          td_->videos_manager_->merge_videos(new_file_id, content->file_id, false);
          content->file_id = new_file_id;
          is_content_changed = true;
        }
        break;
      }
      case MessageVideoNote::ID: {
        auto content = static_cast<MessageVideoNote *>(sent_message->content.get());
        if (new_file_id != content->file_id) {
          td_->video_notes_manager_->merge_video_notes(new_file_id, content->file_id, false);
          content->file_id = new_file_id;
          is_content_changed = true;
        }
        break;
      }
      case MessageVoiceNote::ID: {
        auto content = static_cast<MessageVoiceNote *>(sent_message->content.get());
        if (new_file_id != content->file_id) {
          td_->voice_notes_manager_->merge_voice_notes(new_file_id, content->file_id, false);
          content->file_id = new_file_id;
          is_content_changed = true;
        }
        break;
      }
      case MessageContact::ID:
      case MessageGame::ID:
      case MessageInvoice::ID:
      case MessageLiveLocation::ID:
      case MessageLocation::ID:
      case MessageText::ID:
      case MessageVenue::ID:
      case MessageChatCreate::ID:
      case MessageChatChangeTitle::ID:
      case MessageChatChangePhoto::ID:
      case MessageChatDeletePhoto::ID:
      case MessageChatDeleteHistory::ID:
      case MessageChatAddUsers::ID:
      case MessageChatJoinedByLink::ID:
      case MessageChatDeleteUser::ID:
      case MessageChatMigrateTo::ID:
      case MessageChannelCreate::ID:
      case MessageChannelMigrateFrom::ID:
      case MessagePinMessage::ID:
      case MessageGameScore::ID:
      case MessageScreenshotTaken::ID:
      case MessageChatSetTtl::ID:
      case MessageUnsupported::ID:
      case MessageCall::ID:
      case MessagePaymentSuccessful::ID:
      case MessageContactRegistered::ID:
      case MessageExpiredPhoto::ID:
      case MessageExpiredVideo::ID:
      case MessageCustomServiceAction::ID:
      case MessageWebsiteConnected::ID:
        LOG(ERROR) << "Receive new file " << new_file_id << " in a sent message of the type " << content_type;
        break;
      default:
        UNREACHABLE();
        break;
    }
  }
  if (is_content_changed) {
    send_update_message_content(dialog_id, old_message_id, sent_message->content.get(), sent_message->date,
                                sent_message->is_content_secret, source);
  }

  sent_message->message_id = new_message_id;
  sent_message->random_y = get_random_y(sent_message->message_id);

  sent_message->have_previous = true;
  sent_message->have_next = true;

  bool need_update = true;
  Message *m = add_message_to_dialog(d, std::move(sent_message), true, &need_update, &need_update_dialog_pos, source);
  CHECK(m != nullptr);

  send_update_message_send_succeeded(d, old_message_id, m);
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "on_send_message_success");
  }
  try_add_active_live_location(dialog_id, m);
  return {dialog_id, new_message_id};
}

void MessagesManager::on_send_message_file_part_missing(int64 random_id, int bad_part) {
  auto it = being_sent_messages_.find(random_id);
  if (it == being_sent_messages_.end()) {
    // we can't receive fail more than once
    // but message can be successfully sent before
    LOG(WARNING) << "Receive FILE_PART_" << bad_part
                 << "_MISSING about successfully sent message with random_id = " << random_id;
    return;
  }

  auto full_message_id = it->second;

  being_sent_messages_.erase(it);

  Message *m = get_message(full_message_id);
  if (m == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel
    // don't need to send error to the user, because the message has already been deleted
    // and there is nothing to be deleted from the server
    LOG(INFO) << "Fail to send already deleted by the user or sent to inaccessible chat " << full_message_id;
    return;
  }

  auto dialog_id = full_message_id.get_dialog_id();
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // LOG(ERROR) << "Found " << m->message_id << " in inaccessible " << dialog_id;
    // dump_debug_message_op(get_dialog(dialog_id), 5);
  }

  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  if (dialog_id.get_type() == DialogType::SecretChat) {
    // need to change message random_id before resending
    do {
      m->random_id = Random::secure_int64();
    } while (m->random_id == 0 || message_random_ids_.find(m->random_id) != message_random_ids_.end());
    message_random_ids_.insert(m->random_id);

    LOG(INFO) << "Replace random_id from " << random_id << " to " << m->random_id << " in " << m->message_id << " in "
              << dialog_id;
    d->random_id_to_message_id.erase(random_id);
    d->random_id_to_message_id[m->random_id] = m->message_id;

    auto logevent = SendMessageLogEvent(dialog_id, m);
    auto storer = LogEventStorerImpl<SendMessageLogEvent>(logevent);
    CHECK(m->send_message_logevent_id != 0);
    BinlogHelper::rewrite(G()->td_db()->get_binlog(), m->send_message_logevent_id, LogEvent::HandlerType::SendMessage,
                          storer);
  }

  do_send_message(dialog_id, m, {bad_part});
}

void MessagesManager::on_send_message_fail(int64 random_id, Status error) {
  CHECK(error.is_error());

  auto it = being_sent_messages_.find(random_id);
  if (it == being_sent_messages_.end()) {
    // we can't receive fail more than once
    // but message can be successfully sent before
    if (error.code() != NetQuery::Cancelled) {
      auto debug_it = debug_being_sent_messages_.find(random_id);
      if (debug_it == debug_being_sent_messages_.end()) {
        LOG(ERROR) << "Message with random_id " << random_id << " was not sent";
        return;
      }
      auto dialog_id = debug_it->second;
      if (!dialog_id.is_valid()) {
        LOG(ERROR) << "Sent message is in invalid " << dialog_id;
        return;
      }
      if (!have_dialog(dialog_id)) {
        LOG(ERROR) << "Sent message is in not found " << dialog_id;
        return;
      }
      LOG(ERROR) << "Receive error " << error << " about successfully sent message with random_id = " << random_id
                 << " in " << dialog_id;
      dump_debug_message_op(get_dialog(dialog_id), 7);
    }
    return;
  }

  auto full_message_id = it->second;

  being_sent_messages_.erase(it);

  Message *m = get_message(full_message_id);
  if (m == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel
    // don't need to send error to the user, because the message has already been deleted
    // and there is nothing to be deleted from the server
    LOG(INFO) << "Fail to send already deleted by the user or sent to inaccessible chat " << full_message_id;
    return;
  }
  LOG_IF(ERROR, error.code() == NetQuery::Cancelled)
      << "Receive error " << error << " about sent message with random_id = " << random_id;

  auto dialog_id = full_message_id.get_dialog_id();
  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // LOG(ERROR) << "Found " << m->message_id << " in inaccessible " << dialog_id;
    // dump_debug_message_op(get_dialog(dialog_id), 5);
  }

  int error_code = error.code();
  string error_message = error.message().str();
  switch (error_code) {
    case 420:
      error_code = 429;
      LOG(ERROR) << "Receive error 420: " << error_message;
      break;
    case 429:
      // nothing special, error description has already been changed
      LOG_IF(ERROR, !begins_with(error_message, "Too Many Requests: retry after "))
          << "Wrong error message: " << error_message;
      break;
    case 400:
      if (error.message() == "MESSAGE_TOO_LONG") {
        error_message = "Message is too long";
        // TODO move check to send_message
      } else if (error.message() == "INPUT_USER_DEACTIVATED") {
        error_code = 403;
        error_message = "User is deactivated";
      } else if (error.message() == "USER_IS_BLOCKED") {
        error_code = 403;
        if (td_->auth_manager_->is_bot()) {
          switch (dialog_id.get_type()) {
            case DialogType::User:
              error_message = "Bot was blocked by the user";
              break;
            case DialogType::Chat:
            case DialogType::Channel:
              error_message = "Bot was kicked from the chat";
              break;
            case DialogType::SecretChat:
              break;
            case DialogType::None:
            default:
              UNREACHABLE();
          }
        } else {
          switch (dialog_id.get_type()) {
            case DialogType::User:
              error_message = "User was blocked by the other user";
              break;
            case DialogType::Chat:
            case DialogType::Channel:
              error_message = "User is not in the chat";
              break;
            case DialogType::SecretChat:
              break;
            case DialogType::None:
            default:
              UNREACHABLE();
          }
        }
        // TODO add check to send_message
      } else if (error.message() == "USER_IS_BOT") {
        error_code = 403;
        error_message = "Bot can't send messages to bots";
        // TODO move check to send_message
      } else if (error.message() == "PEER_ID_INVALID") {
        error_code = 403;
        error_message = "Bot can't initiate conversation with a user";
      } else if (error.message() == "WC_CONVERT_URL_INVALID" || error.message() == "EXTERNAL_URL_INVALID") {
        error_message = "Wrong HTTP URL specified";
      } else if (error.message() == "WEBPAGE_CURL_FAILED") {
        error_message = "Failed to get HTTP URL content";
      } else if (error.message() == "WEBPAGE_MEDIA_EMPTY") {
        error_message = "Wrong type of the web page content";
      } else if (error.message() == "MEDIA_EMPTY") {
        auto content_type = m->content->get_id();
        if (content_type == MessageGame::ID) {
          error_message = "Wrong game short name specified";
        } else if (content_type == MessageInvoice::ID) {
          error_message = "Wrong invoice information specified";
        } else {
          error_message = "Wrong file identifier/HTTP URL specified";
        }
      } else if (error.message() == "PHOTO_EXT_INVALID") {
        error_message = "Photo has unsupported extension. Use one of .jpg, .jpeg, .gif, .png, .tif or .bmp";
      }
      break;
    case 403:
      if (error.message() == "MESSAGE_DELETE_FORBIDDEN") {
        error_code = 400;
        error_message = "Message can't be deleted";
      } else if (error.message() != "CHANNEL_PUBLIC_GROUP_NA" && error.message() != "USER_IS_BLOCKED" &&
                 error.message() != "USER_BOT_INVALID" && error.message() != "USER_DELETED") {
        error_code = 400;
      }
      break;
    // TODO other codes
    default:
      break;
  }
  if (error.message() == "REPLY_MARKUP_INVALID") {
    if (m->reply_markup == nullptr) {
      LOG(ERROR) << "Receive " << error.message() << " for " << oneline(to_string(get_message_object(dialog_id, m)));
    } else {
      LOG(ERROR) << "Receive " << error.message() << " for " << full_message_id << " with keyboard "
                 << *m->reply_markup;
    }
  }
  LOG_IF(WARNING, error_code != 403) << "Fail to send " << full_message_id << " with the error " << error;
  if (error_code <= 0) {
    error_code = 500;
  }
  fail_send_message(full_message_id, error_code, error_message);
}

MessageId MessagesManager::get_next_message_id(Dialog *d, int32 type) {
  CHECK(d != nullptr);
  int64 last = std::max({d->last_message_id.get(), d->last_new_message_id.get(), d->last_database_message_id.get(),
                         d->last_assigned_message_id.get(), d->last_clear_history_message_id.get(),
                         d->deleted_last_message_id.get(), d->max_unavailable_message_id.get()});
  if (last < d->last_read_inbox_message_id.get() &&
      d->last_read_inbox_message_id.get() < d->last_new_message_id.get() + MessageId::FULL_TYPE_MASK) {
    last = d->last_read_inbox_message_id.get();
  }
  if (last < d->last_read_outbox_message_id.get() &&
      d->last_read_outbox_message_id.get() < d->last_new_message_id.get() + MessageId::FULL_TYPE_MASK) {
    last = d->last_read_outbox_message_id.get();
  }

  int64 base = (last + MessageId::TYPE_MASK + 1) & ~MessageId::TYPE_MASK;
  d->last_assigned_message_id = MessageId(base + type);
  return d->last_assigned_message_id;
}

MessageId MessagesManager::get_next_yet_unsent_message_id(Dialog *d) {
  return get_next_message_id(d, MessageId::TYPE_YET_UNSENT);
}

MessageId MessagesManager::get_next_local_message_id(Dialog *d) {
  return get_next_message_id(d, MessageId::TYPE_LOCAL);
}

void MessagesManager::fail_send_message(FullMessageId full_message_id, int error_code, const string &error_message) {
  auto dialog_id = full_message_id.get_dialog_id();
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  MessageId old_message_id = full_message_id.get_message_id();
  CHECK(old_message_id.is_yet_unsent());

  bool need_update_dialog_pos = false;
  unique_ptr<Message> message = delete_message(d, old_message_id, false, &need_update_dialog_pos, "fail send message");
  if (message == nullptr) {
    // message has already been deleted by the user or sent to inaccessible channel
    // don't need to send update to the user, because the message has already been deleted
    // and there is nothing to be deleted from the server
    return;
  }

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    // LOG(ERROR) << "Found " << old_message_id << " in inaccessible " << dialog_id;
    // dump_debug_message_op(d, 5);
  }

  auto new_message_id = MessageId(old_message_id.get() - MessageId::TYPE_YET_UNSENT + MessageId::TYPE_LOCAL);
  if (get_message_force(d, new_message_id) != nullptr || d->deleted_message_ids.count(new_message_id)) {
    new_message_id = get_next_local_message_id(d);
  }

  message->message_id = new_message_id;
  CHECK(message->message_id.is_valid());
  message->random_y = get_random_y(message->message_id);
  message->is_failed_to_send = true;

  message->have_previous = true;
  message->have_next = true;

  bool need_update = false;
  Message *m = add_message_to_dialog(dialog_id, std::move(message), false, &need_update, &need_update_dialog_pos,
                                     "fail_send_message");
  CHECK(m != nullptr) << "Failed to add failed to send " << new_message_id << " to " << dialog_id << " due to "
                      << debug_add_message_to_dialog_fail_reason;

  LOG(INFO) << "Send updateMessageSendFailed for " << full_message_id;
  d->yet_unsent_message_id_to_persistent_message_id.emplace(old_message_id, m->message_id);
  send_closure(G()->td(), &Td::send_update,
               make_tl_object<td_api::updateMessageSendFailed>(get_message_object(dialog_id, m), old_message_id.get(),
                                                               error_code, error_message));
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "fail_send_message");
  }
}

void MessagesManager::on_update_dialog_draft_message(DialogId dialog_id,
                                                     tl_object_ptr<telegram_api::DraftMessage> &&draft_message) {
  if (!dialog_id.is_valid()) {
    LOG(ERROR) << "Receive update chat draft in invalid " << dialog_id;
    return;
  }
  auto d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(INFO) << "Ignore update chat draft in unknown " << dialog_id;
    return;
  }
  update_dialog_draft_message(d, get_draft_message(td_->contacts_manager_.get(), std::move(draft_message)), true, true);
}

bool MessagesManager::update_dialog_draft_message(Dialog *d, unique_ptr<DraftMessage> &&draft_message, bool from_update,
                                                  bool need_update_dialog_pos) {
  CHECK(d != nullptr);
  if (from_update && d->is_opened && d->draft_message != nullptr) {
    // send the update anyway, despite it shouldn't be applied client-side
    // return false;
  }
  if (draft_message == nullptr) {
    if (d->draft_message != nullptr) {
      d->draft_message = nullptr;
      if (need_update_dialog_pos) {
        update_dialog_pos(d, false, "update_dialog_draft_message", false);
      }
      send_update_chat_draft_message(d);
      return true;
    }
  } else {
    if (d->draft_message != nullptr && d->draft_message->reply_to_message_id == draft_message->reply_to_message_id &&
        d->draft_message->input_message_text == draft_message->input_message_text) {
      if (d->draft_message->date < draft_message->date) {
        if (need_update_dialog_pos) {
          update_dialog_pos(d, false, "update_dialog_draft_message 2");
        }
        d->draft_message->date = draft_message->date;
        return true;
      }
    } else {
      if (!from_update || d->draft_message == nullptr || d->draft_message->date <= draft_message->date) {
        d->draft_message = std::move(draft_message);
        if (need_update_dialog_pos) {
          update_dialog_pos(d, false, "update_dialog_draft_message 3", false);
        }
        send_update_chat_draft_message(d);
        return true;
      }
    }
  }
  return false;
}

void MessagesManager::on_update_dialog_pinned(DialogId dialog_id, bool is_pinned) {
  if (!dialog_id.is_valid()) {
    LOG(ERROR) << "Receive pinn of invalid " << dialog_id;
    return;
  }
  auto d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(WARNING) << "Can't apply updateDialogPinned with " << dialog_id;
    // TODO logevent + promise
    send_closure(td_->create_net_actor<GetPinnedDialogsQuery>(Promise<>()), &GetPinnedDialogsQuery::send,
                 get_sequence_dispatcher_id(DialogId(), -1));
    return;
  }
  if (!is_pinned && d->pinned_order == DEFAULT_ORDER) {
    return;
  }
  set_dialog_is_pinned(d, is_pinned);
  update_dialog_pos(d, false, "on_update_dialog_pinned");
}

void MessagesManager::on_update_pinned_dialogs() {
  // TODO logevent + promise
  send_closure(td_->create_net_actor<GetPinnedDialogsQuery>(Promise<>()), &GetPinnedDialogsQuery::send,
               get_sequence_dispatcher_id(DialogId(), -1));
}

void MessagesManager::on_create_new_dialog_success(int64 random_id, tl_object_ptr<telegram_api::Updates> &&updates,
                                                   DialogType expected_type, Promise<Unit> &&promise) {
  auto sent_messages = td_->updates_manager_->get_new_messages(updates.get());
  auto sent_messages_random_ids = td_->updates_manager_->get_sent_messages_random_ids(updates.get());
  if (sent_messages.size() != 1u || sent_messages_random_ids.size() != 1u) {
    LOG(ERROR) << "Receive wrong result for create group or channel chat " << oneline(to_string(updates));
    return on_create_new_dialog_fail(random_id, Status::Error(500, "Unsupported server response"), std::move(promise));
  }

  auto message = *sent_messages.begin();
  // int64 message_random_id = *sent_messages_random_ids.begin();
  // TODO check that message_random_id equals random_id after messages_createChat will be updated

  auto dialog_id = get_message_dialog_id(*message);
  if (dialog_id.get_type() != expected_type) {
    return on_create_new_dialog_fail(random_id, Status::Error(500, "Chat of wrong type has been created"),
                                     std::move(promise));
  }

  auto it = created_dialogs_.find(random_id);
  CHECK(it != created_dialogs_.end());
  CHECK(it->second == DialogId());

  it->second = dialog_id;

  const Dialog *d = get_dialog(dialog_id);
  if (d != nullptr && d->last_new_message_id.is_valid()) {
    // dialog have been already created and at least one non-temporary message was added,
    // i.e. we are not interested in the creation of dialog by searchMessages
    // then messages have already been added, so just set promise
    return promise.set_value(Unit());
  }

  if (pending_created_dialogs_.find(dialog_id) == pending_created_dialogs_.end()) {
    pending_created_dialogs_.emplace(dialog_id, std::move(promise));
  } else {
    LOG(ERROR) << dialog_id << " returned twice as result of chat creation";
    return on_create_new_dialog_fail(random_id, Status::Error(500, "Chat was created earlier"), std::move(promise));
  }

  td_->updates_manager_->on_get_updates(std::move(updates));
}

void MessagesManager::on_create_new_dialog_fail(int64 random_id, Status error, Promise<Unit> &&promise) {
  LOG(INFO) << "Clean up creation of group or channel chat";
  auto it = created_dialogs_.find(random_id);
  CHECK(it != created_dialogs_.end());
  CHECK(it->second == DialogId());
  created_dialogs_.erase(it);

  CHECK(error.is_error());
  promise.set_error(std::move(error));

  // repairing state by running get difference
  td_->updates_manager_->get_difference("on_create_new_dialog_fail");
}

void MessagesManager::on_dialog_photo_updated(DialogId dialog_id) {
  if (have_dialog(dialog_id)) {
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateChatPhoto>(
                     dialog_id.get(), get_chat_photo_object(td_->file_manager_.get(), get_dialog_photo(dialog_id))));
  }
}

void MessagesManager::on_dialog_title_updated(DialogId dialog_id) {
  auto d = get_dialog(dialog_id);
  if (d != nullptr) {
    update_dialogs_hints(d);
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateChatTitle>(dialog_id.get(), get_dialog_title(dialog_id)));
  }
}

DialogId MessagesManager::resolve_dialog_username(const string &username) {
  auto it = resolved_usernames_.find(clean_username(username));
  if (it != resolved_usernames_.end()) {
    return it->second.dialog_id;
  }

  auto it2 = unaccessible_resolved_usernames_.find(clean_username(username));
  if (it2 != unaccessible_resolved_usernames_.end()) {
    return it2->second;
  }

  return DialogId();
}

DialogId MessagesManager::search_public_dialog(const string &username_to_search, bool force, Promise<Unit> &&promise) {
  string username = clean_username(username_to_search);
  if (username[0] == '@') {
    username = username.substr(1);
  }
  if (username.empty()) {
    promise.set_error(Status::Error(200, "Username is invalid"));
    return DialogId();
  }

  DialogId dialog_id;
  auto it = resolved_usernames_.find(username);
  if (it != resolved_usernames_.end()) {
    if (it->second.expires_at < Time::now()) {
      td_->create_handler<ResolveUsernameQuery>(Promise<>())->send(username);
    }
    dialog_id = it->second.dialog_id;
  } else {
    auto it2 = unaccessible_resolved_usernames_.find(username);
    if (it2 != unaccessible_resolved_usernames_.end()) {
      dialog_id = it2->second;
    }
  }

  if (dialog_id.is_valid()) {
    if (have_input_peer(dialog_id, AccessRights::Read)) {
      if (td_->auth_manager_->is_bot()) {
        force_create_dialog(dialog_id, "search public dialog");
      } else {
        const Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !d->notification_settings.is_synchronized) {
          send_get_dialog_query(dialog_id, std::move(promise));
          return DialogId();
        }
      }

      promise.set_value(Unit());
      return dialog_id;
    } else {
      // bot username maybe known despite there is no access_hash
      if (force || dialog_id.get_type() != DialogType::User) {
        force_create_dialog(dialog_id, "search public dialog");
        promise.set_value(Unit());
        return dialog_id;
      }
    }
  }

  td_->create_handler<ResolveUsernameQuery>(std::move(promise))->send(username);
  return DialogId();
}

void MessagesManager::send_get_dialog_query(DialogId dialog_id, Promise<Unit> &&promise) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  auto &promises = get_dialog_queries_[dialog_id];
  promises.push_back(std::move(promise));
  if (promises.size() != 1) {
    // query has already been sent, just wait for the result
    return;
  }

  td_->create_handler<GetDialogQuery>()->send(dialog_id);
}

void MessagesManager::on_get_dialog_success(DialogId dialog_id) {
  auto it = get_dialog_queries_.find(dialog_id);
  CHECK(it != get_dialog_queries_.end());
  CHECK(it->second.size() > 0);
  auto promises = std::move(it->second);
  get_dialog_queries_.erase(it);

  for (auto &promise : promises) {
    promise.set_value(Unit());
  }
}

void MessagesManager::on_get_dialog_fail(DialogId dialog_id, Status &&error) {
  auto it = get_dialog_queries_.find(dialog_id);
  CHECK(it != get_dialog_queries_.end());
  CHECK(it->second.size() > 0);
  auto promises = std::move(it->second);
  get_dialog_queries_.erase(it);

  for (auto &promise : promises) {
    promise.set_error(error.clone());
  }
}

bool MessagesManager::is_update_about_username_change_received(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      return td_->contacts_manager_->is_update_about_username_change_received(dialog_id.get_user_id());
    case DialogType::Chat:
      return true;
    case DialogType::Channel: {
      auto status = td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id());
      return status.is_member();
    }
    case DialogType::SecretChat:
      return true;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }
}

void MessagesManager::on_dialog_username_updated(DialogId dialog_id, const string &old_username,
                                                 const string &new_username) {
  auto d = get_dialog(dialog_id);
  if (d != nullptr) {
    update_dialogs_hints(d);
  }
  if (!old_username.empty() && old_username != new_username) {
    resolved_usernames_.erase(clean_username(old_username));
    unaccessible_resolved_usernames_.erase(clean_username(old_username));
  }
  if (!new_username.empty()) {
    auto cache_time = is_update_about_username_change_received(dialog_id) ? USERNAME_CACHE_EXPIRE_TIME
                                                                          : USERNAME_CACHE_EXPIRE_TIME_SHORT;
    resolved_usernames_[clean_username(new_username)] = ResolvedUsername{dialog_id, Time::now() + cache_time};
  }
}

void MessagesManager::on_resolved_username(const string &username, DialogId dialog_id) {
  if (!dialog_id.is_valid()) {
    LOG(ERROR) << "Resolve username \"" << username << "\" to invalid " << dialog_id;
    return;
  }

  auto it = resolved_usernames_.find(clean_username(username));
  if (it != resolved_usernames_.end()) {
    LOG_IF(ERROR, it->second.dialog_id != dialog_id)
        << "Resolve username \"" << username << "\" to " << dialog_id << ", but have it in " << it->second.dialog_id;
    return;
  }

  unaccessible_resolved_usernames_[clean_username(username)] = dialog_id;
}

void MessagesManager::drop_username(const string &username) {
  unaccessible_resolved_usernames_.erase(clean_username(username));

  auto it = resolved_usernames_.find(clean_username(username));
  if (it == resolved_usernames_.end()) {
    return;
  }

  auto dialog_id = it->second.dialog_id;
  if (have_input_peer(dialog_id, AccessRights::Read)) {
    CHECK(dialog_id.get_type() != DialogType::SecretChat);
    send_get_dialog_query(dialog_id, Auto());
  }

  resolved_usernames_.erase(it);
}

const DialogPhoto *MessagesManager::get_dialog_photo(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      return td_->contacts_manager_->get_user_dialog_photo(dialog_id.get_user_id());
    case DialogType::Chat:
      return td_->contacts_manager_->get_chat_dialog_photo(dialog_id.get_chat_id());
    case DialogType::Channel:
      return td_->contacts_manager_->get_channel_dialog_photo(dialog_id.get_channel_id());
    case DialogType::SecretChat:
      return td_->contacts_manager_->get_secret_chat_dialog_photo(dialog_id.get_secret_chat_id());
    case DialogType::None:
    default:
      UNREACHABLE();
      return nullptr;
  }
}

string MessagesManager::get_dialog_title(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      return td_->contacts_manager_->get_user_title(dialog_id.get_user_id());
    case DialogType::Chat:
      return td_->contacts_manager_->get_chat_title(dialog_id.get_chat_id());
    case DialogType::Channel:
      return td_->contacts_manager_->get_channel_title(dialog_id.get_channel_id());
    case DialogType::SecretChat:
      return td_->contacts_manager_->get_secret_chat_title(dialog_id.get_secret_chat_id());
    case DialogType::None:
    default:
      UNREACHABLE();
      return string();
  }
}

string MessagesManager::get_dialog_username(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      return td_->contacts_manager_->get_user_username(dialog_id.get_user_id());
    case DialogType::Chat:
      return string();
    case DialogType::Channel:
      return td_->contacts_manager_->get_channel_username(dialog_id.get_channel_id());
    case DialogType::SecretChat:
      return td_->contacts_manager_->get_secret_chat_username(dialog_id.get_secret_chat_id());
    case DialogType::None:
    default:
      UNREACHABLE();
      return string();
  }
}

void MessagesManager::send_dialog_action(DialogId dialog_id, const tl_object_ptr<td_api::ChatAction> &action,
                                         Promise<Unit> &&promise) {
  if (action == nullptr) {
    return promise.set_error(Status::Error(5, "Action must not be empty"));
  }

  if (!have_dialog_force(dialog_id)) {
    return promise.set_error(Status::Error(5, "Chat not found"));
  }

  auto can_send_status = can_send_message(dialog_id);
  if (can_send_status.is_error()) {
    return promise.set_error(can_send_status.move_as_error());
  }

  if (dialog_id.get_type() == DialogType::SecretChat) {
    tl_object_ptr<secret_api::SendMessageAction> send_action;
    switch (action->get_id()) {
      case td_api::chatActionCancel::ID:
        send_action = make_tl_object<secret_api::sendMessageCancelAction>();
        break;
      case td_api::chatActionTyping::ID:
        send_action = make_tl_object<secret_api::sendMessageTypingAction>();
        break;
      case td_api::chatActionRecordingVideo::ID:
        send_action = make_tl_object<secret_api::sendMessageRecordVideoAction>();
        break;
      case td_api::chatActionUploadingVideo::ID:
        send_action = make_tl_object<secret_api::sendMessageUploadVideoAction>();
        break;
      case td_api::chatActionRecordingVoiceNote::ID:
        send_action = make_tl_object<secret_api::sendMessageRecordAudioAction>();
        break;
      case td_api::chatActionUploadingVoiceNote::ID:
        send_action = make_tl_object<secret_api::sendMessageUploadAudioAction>();
        break;
      case td_api::chatActionUploadingPhoto::ID:
        send_action = make_tl_object<secret_api::sendMessageUploadPhotoAction>();
        break;
      case td_api::chatActionUploadingDocument::ID:
        send_action = make_tl_object<secret_api::sendMessageUploadDocumentAction>();
        break;
      case td_api::chatActionChoosingLocation::ID:
        send_action = make_tl_object<secret_api::sendMessageGeoLocationAction>();
        break;
      case td_api::chatActionChoosingContact::ID:
        send_action = make_tl_object<secret_api::sendMessageChooseContactAction>();
        break;
      case td_api::chatActionRecordingVideoNote::ID:
        send_action = make_tl_object<secret_api::sendMessageRecordRoundAction>();
        break;
      case td_api::chatActionUploadingVideoNote::ID:
        send_action = make_tl_object<secret_api::sendMessageUploadRoundAction>();
        break;
      case td_api::chatActionStartPlayingGame::ID:
        return promise.set_error(Status::Error(5, "Games are unsupported in secret chats"));
      default:
        UNREACHABLE();
    }
    send_closure(G()->secret_chats_manager(), &SecretChatsManager::send_message_action, dialog_id.get_secret_chat_id(),
                 std::move(send_action));
    promise.set_value(Unit());
    return;
  }

  tl_object_ptr<telegram_api::SendMessageAction> send_action;
  switch (action->get_id()) {
    case td_api::chatActionCancel::ID:
      send_action = make_tl_object<telegram_api::sendMessageCancelAction>();
      break;
    case td_api::chatActionTyping::ID:
      send_action = make_tl_object<telegram_api::sendMessageTypingAction>();
      break;
    case td_api::chatActionRecordingVideo::ID:
      send_action = make_tl_object<telegram_api::sendMessageRecordVideoAction>();
      break;
    case td_api::chatActionUploadingVideo::ID: {
      auto progress = static_cast<td_api::chatActionUploadingVideo &>(*action).progress_;
      send_action = make_tl_object<telegram_api::sendMessageUploadVideoAction>(progress);
      break;
    }
    case td_api::chatActionRecordingVoiceNote::ID:
      send_action = make_tl_object<telegram_api::sendMessageRecordAudioAction>();
      break;
    case td_api::chatActionUploadingVoiceNote::ID: {
      auto progress = static_cast<td_api::chatActionUploadingVoiceNote &>(*action).progress_;
      send_action = make_tl_object<telegram_api::sendMessageUploadAudioAction>(progress);
      break;
    }
    case td_api::chatActionUploadingPhoto::ID: {
      auto progress = static_cast<td_api::chatActionUploadingPhoto &>(*action).progress_;
      send_action = make_tl_object<telegram_api::sendMessageUploadPhotoAction>(progress);
      break;
    }
    case td_api::chatActionUploadingDocument::ID: {
      auto progress = static_cast<td_api::chatActionUploadingDocument &>(*action).progress_;
      send_action = make_tl_object<telegram_api::sendMessageUploadDocumentAction>(progress);
      break;
    }
    case td_api::chatActionChoosingLocation::ID:
      send_action = make_tl_object<telegram_api::sendMessageGeoLocationAction>();
      break;
    case td_api::chatActionChoosingContact::ID:
      send_action = make_tl_object<telegram_api::sendMessageChooseContactAction>();
      break;
    case td_api::chatActionStartPlayingGame::ID:
      send_action = make_tl_object<telegram_api::sendMessageGamePlayAction>();
      break;
    case td_api::chatActionRecordingVideoNote::ID:
      send_action = make_tl_object<telegram_api::sendMessageRecordRoundAction>();
      break;
    case td_api::chatActionUploadingVideoNote::ID: {
      auto progress = static_cast<td_api::chatActionUploadingVideoNote &>(*action).progress_;
      send_action = make_tl_object<telegram_api::sendMessageUploadRoundAction>(progress);
      break;
    }
    default:
      UNREACHABLE();
  }

  auto &query_ref = set_typing_query_[dialog_id];
  if (!query_ref.empty() && !td_->auth_manager_->is_bot()) {
    LOG(INFO) << "Cancel previous set typing query";
    cancel_query(query_ref);
  }
  query_ref = td_->create_handler<SetTypingQuery>(std::move(promise))->send(dialog_id, std::move(send_action));
}

void MessagesManager::on_send_dialog_action_timeout(DialogId dialog_id) {
  LOG(INFO) << "Receive send_dialog_action timeout in " << dialog_id;
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  if (can_send_message(dialog_id).is_error()) {
    return;
  }

  auto queue_id = get_sequence_dispatcher_id(dialog_id, MessagePhoto::ID);
  CHECK(queue_id & 1);

  auto queue_it = yet_unsent_media_queues_.find(queue_id);
  if (queue_it == yet_unsent_media_queues_.end()) {
    return;
  }

  pending_send_dialog_action_timeout_.add_timeout_in(dialog_id.get(), 4.0);

  CHECK(!queue_it->second.empty());
  MessageId message_id(queue_it->second.begin()->first);
  const Message *m = get_message(d, message_id);
  if (m == nullptr) {
    return;
  }
  if (m->forward_info != nullptr) {
    return;
  }

  auto file_id = get_message_content_file_id(m->content.get());
  if (!file_id.is_valid()) {
    LOG(ERROR) << "Have no file in "
               << to_string(get_message_content_object(m->content.get(), m->date, m->is_content_secret));
    return;
  }
  auto file_view = td_->file_manager_->get_file_view(file_id);
  if (!file_view.is_uploading()) {
    return;
  }
  int64 total_size = file_view.expected_size();
  int64 uploaded_size = file_view.remote_size();
  int32 progress = 0;
  if (total_size > 0 && uploaded_size > 0) {
    if (uploaded_size > total_size) {
      uploaded_size = total_size;  // just in case
    }
    progress = static_cast<int32>(100 * uploaded_size / total_size);
  }

  td_api::object_ptr<td_api::ChatAction> action;
  switch (m->content->get_id()) {
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageDocument::ID:
      action = td_api::make_object<td_api::chatActionUploadingDocument>(progress);
      break;
    case MessagePhoto::ID:
      action = td_api::make_object<td_api::chatActionUploadingPhoto>(progress);
      break;
    case MessageVideo::ID:
      action = td_api::make_object<td_api::chatActionUploadingVideo>(progress);
      break;
    case MessageVideoNote::ID:
      action = td_api::make_object<td_api::chatActionUploadingVideoNote>(progress);
      break;
    case MessageVoiceNote::ID:
      action = td_api::make_object<td_api::chatActionUploadingVoiceNote>(progress);
      break;
    default:
      return;
  }
  CHECK(action != nullptr);
  LOG(INFO) << "Send action in " << dialog_id << ": " << to_string(action);
  send_dialog_action(dialog_id, std::move(action), Auto());
}

void MessagesManager::on_active_dialog_action_timeout(DialogId dialog_id) {
  LOG(DEBUG) << "Receive active dialog action timeout in " << dialog_id;
  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  auto actions_it = active_dialog_actions_.find(dialog_id);
  if (actions_it == active_dialog_actions_.end()) {
    return;
  }
  CHECK(!actions_it->second.empty());

  auto now = Time::now();
  while (actions_it->second[0].start_time + DIALOG_ACTION_TIMEOUT < now + 0.1) {
    on_user_dialog_action(dialog_id, actions_it->second[0].user_id, nullptr);

    actions_it = active_dialog_actions_.find(dialog_id);
    if (actions_it == active_dialog_actions_.end()) {
      return;
    }
    CHECK(!actions_it->second.empty());
  }

  LOG(DEBUG) << "Schedule next action timeout in " << dialog_id;
  active_dialog_action_timeout_.add_timeout_in(dialog_id.get(),
                                               actions_it->second[0].start_time + DIALOG_ACTION_TIMEOUT - now);
}

tl_object_ptr<telegram_api::InputChatPhoto> MessagesManager::get_input_chat_photo(FileId file_id) const {
  if (!file_id.is_valid()) {
    return make_tl_object<telegram_api::inputChatPhotoEmpty>();
  }

  FileView file_view = td_->file_manager_->get_file_view(file_id);
  CHECK(!file_view.is_encrypted());
  if (file_view.has_remote_location()) {
    if (file_view.remote_location().is_web()) {
      return nullptr;
    }
    return make_tl_object<telegram_api::inputChatPhoto>(file_view.remote_location().as_input_photo());
  }

  return nullptr;
}

tl_object_ptr<telegram_api::MessagesFilter> MessagesManager::get_input_messages_filter(SearchMessagesFilter filter) {
  switch (filter) {
    case SearchMessagesFilter::Empty:
      return make_tl_object<telegram_api::inputMessagesFilterEmpty>();
    case SearchMessagesFilter::Animation:
      return make_tl_object<telegram_api::inputMessagesFilterGif>();
    case SearchMessagesFilter::Audio:
      return make_tl_object<telegram_api::inputMessagesFilterMusic>();
    case SearchMessagesFilter::Document:
      return make_tl_object<telegram_api::inputMessagesFilterDocument>();
    case SearchMessagesFilter::Photo:
      return make_tl_object<telegram_api::inputMessagesFilterPhotos>();
    case SearchMessagesFilter::Video:
      return make_tl_object<telegram_api::inputMessagesFilterVideo>();
    case SearchMessagesFilter::VoiceNote:
      return make_tl_object<telegram_api::inputMessagesFilterVoice>();
    case SearchMessagesFilter::PhotoAndVideo:
      return make_tl_object<telegram_api::inputMessagesFilterPhotoVideo>();
    case SearchMessagesFilter::Url:
      return make_tl_object<telegram_api::inputMessagesFilterUrl>();
    case SearchMessagesFilter::ChatPhoto:
      return make_tl_object<telegram_api::inputMessagesFilterChatPhotos>();
    case SearchMessagesFilter::Call:
      return make_tl_object<telegram_api::inputMessagesFilterPhoneCalls>(0, false /*ignored*/);
    case SearchMessagesFilter::MissedCall:
      return make_tl_object<telegram_api::inputMessagesFilterPhoneCalls>(
          telegram_api::inputMessagesFilterPhoneCalls::MISSED_MASK, false /*ignored*/);
    case SearchMessagesFilter::VideoNote:
      return make_tl_object<telegram_api::inputMessagesFilterRoundVideo>();
    case SearchMessagesFilter::VoiceAndVideoNote:
      return make_tl_object<telegram_api::inputMessagesFilterRoundVoice>();
    case SearchMessagesFilter::Mention:
      return make_tl_object<telegram_api::inputMessagesFilterMyMentions>();
    default:
      UNREACHABLE();
      return nullptr;
  }
}

SearchMessagesFilter MessagesManager::get_search_messages_filter(
    const tl_object_ptr<td_api::SearchMessagesFilter> &filter) {
  if (filter == nullptr) {
    return SearchMessagesFilter::Empty;
  }
  switch (filter->get_id()) {
    case td_api::searchMessagesFilterEmpty::ID:
      return SearchMessagesFilter::Empty;
    case td_api::searchMessagesFilterAnimation::ID:
      return SearchMessagesFilter::Animation;
    case td_api::searchMessagesFilterAudio::ID:
      return SearchMessagesFilter::Audio;
    case td_api::searchMessagesFilterDocument::ID:
      return SearchMessagesFilter::Document;
    case td_api::searchMessagesFilterPhoto::ID:
      return SearchMessagesFilter::Photo;
    case td_api::searchMessagesFilterVideo::ID:
      return SearchMessagesFilter::Video;
    case td_api::searchMessagesFilterVoiceNote::ID:
      return SearchMessagesFilter::VoiceNote;
    case td_api::searchMessagesFilterPhotoAndVideo::ID:
      return SearchMessagesFilter::PhotoAndVideo;
    case td_api::searchMessagesFilterUrl::ID:
      return SearchMessagesFilter::Url;
    case td_api::searchMessagesFilterChatPhoto::ID:
      return SearchMessagesFilter::ChatPhoto;
    case td_api::searchMessagesFilterCall::ID:
      return SearchMessagesFilter::Call;
    case td_api::searchMessagesFilterMissedCall::ID:
      return SearchMessagesFilter::MissedCall;
    case td_api::searchMessagesFilterVideoNote::ID:
      return SearchMessagesFilter::VideoNote;
    case td_api::searchMessagesFilterVoiceAndVideoNote::ID:
      return SearchMessagesFilter::VoiceAndVideoNote;
    case td_api::searchMessagesFilterMention::ID:
      return SearchMessagesFilter::Mention;
    case td_api::searchMessagesFilterUnreadMention::ID:
      return SearchMessagesFilter::UnreadMention;
    default:
      UNREACHABLE();
      return SearchMessagesFilter::Empty;
  }
}

void MessagesManager::set_dialog_photo(DialogId dialog_id, const tl_object_ptr<td_api::InputFile> &photo,
                                       Promise<Unit> &&promise) {
  LOG(INFO) << "Receive SetChatPhoto request to change photo of " << dialog_id;

  if (!have_dialog_force(dialog_id)) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(3, "Can't change private chat photo"));
    case DialogType::Chat: {
      auto chat_id = dialog_id.get_chat_id();
      auto status = td_->contacts_manager_->get_chat_status(chat_id);
      if (!status.can_change_info_and_settings() ||
          (td_->auth_manager_->is_bot() && !td_->contacts_manager_->is_appointed_chat_administrator(chat_id))) {
        return promise.set_error(Status::Error(3, "Not enough rights to change chat photo"));
      }
      break;
    }
    case DialogType::Channel: {
      auto status = td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id());
      if (!status.can_change_info_and_settings()) {
        return promise.set_error(Status::Error(3, "Not enough rights to change chat photo"));
      }
      break;
    }
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(3, "Can't change secret chat photo"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  auto r_file_id = td_->file_manager_->get_input_file_id(FileType::Photo, photo, dialog_id, true, false);
  if (r_file_id.is_error()) {
    return promise.set_error(Status::Error(7, r_file_id.error().message()));
  }
  FileId file_id = r_file_id.ok();

  auto input_chat_photo = get_input_chat_photo(file_id);
  if (input_chat_photo != nullptr) {
    // file has already been uploaded, just send change photo request
    // TODO invoke after
    td_->create_handler<EditDialogPhotoQuery>(std::move(promise))
        ->send(FileId(), dialog_id, std::move(input_chat_photo));
    return;
  }

  // need to upload file first
  auto upload_file_id = td_->file_manager_->dup_file_id(file_id);
  CHECK(upload_file_id.is_valid());
  CHECK(uploaded_dialog_photos_.find(upload_file_id) == uploaded_dialog_photos_.end());
  uploaded_dialog_photos_[upload_file_id] = {std::move(promise), dialog_id};
  LOG(INFO) << "Ask to upload chat photo " << upload_file_id;
  td_->file_manager_->upload(upload_file_id, upload_dialog_photo_callback_, 1, 0);
}

void MessagesManager::set_dialog_title(DialogId dialog_id, const string &title, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive SetChatTitle request to change title of " << dialog_id << " to \"" << title << '"';

  if (!have_dialog_force(dialog_id)) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  auto new_title = clean_name(title, MAX_NAME_LENGTH);
  if (new_title.empty()) {
    return promise.set_error(Status::Error(3, "Title can't be empty"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(3, "Can't change private chat title"));
    case DialogType::Chat: {
      auto chat_id = dialog_id.get_chat_id();
      auto status = td_->contacts_manager_->get_chat_status(chat_id);
      if (!status.can_change_info_and_settings() ||
          (td_->auth_manager_->is_bot() && !td_->contacts_manager_->is_appointed_chat_administrator(chat_id))) {
        return promise.set_error(Status::Error(3, "Not enough rights to change chat title"));
      }
      break;
    }
    case DialogType::Channel: {
      auto status = td_->contacts_manager_->get_channel_status(dialog_id.get_channel_id());
      if (!status.can_change_info_and_settings()) {
        return promise.set_error(Status::Error(3, "Not enough rights to change chat title"));
      }
      break;
    }
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(3, "Can't change secret chat title"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  // TODO this can be wrong if there was previous change title requests
  if (get_dialog_title(dialog_id) == new_title) {
    return promise.set_value(Unit());
  }

  // TODO invoke after
  td_->create_handler<EditDialogTitleQuery>(std::move(promise))->send(dialog_id, new_title);
}

void MessagesManager::add_dialog_participant(DialogId dialog_id, UserId user_id, int32 forward_limit,
                                             Promise<Unit> &&promise) {
  LOG(INFO) << "Receive AddChatParticipant request to add " << user_id << " to " << dialog_id;
  if (!have_dialog_force(dialog_id)) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(3, "Can't add members to a private chat"));
    case DialogType::Chat:
      return td_->contacts_manager_->add_chat_participant(dialog_id.get_chat_id(), user_id, forward_limit,
                                                          std::move(promise));
    case DialogType::Channel:
      return td_->contacts_manager_->add_channel_participant(dialog_id.get_channel_id(), user_id, std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(3, "Can't add members to a secret chat"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void MessagesManager::add_dialog_participants(DialogId dialog_id, const vector<UserId> &user_ids,
                                              Promise<Unit> &&promise) {
  LOG(INFO) << "Receive AddChatParticipants request to add " << format::as_array(user_ids) << " to " << dialog_id;
  if (td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(3, "Method is not available for bots"));
  }

  if (!have_dialog_force(dialog_id)) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(3, "Can't add members to a private chat"));
    case DialogType::Chat:
      return promise.set_error(Status::Error(3, "Can't add many members at once to a basic group chat"));
    case DialogType::Channel:
      return td_->contacts_manager_->add_channel_participants(dialog_id.get_channel_id(), user_ids, std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(3, "Can't add members to a secret chat"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void MessagesManager::set_dialog_participant_status(DialogId dialog_id, UserId user_id,
                                                    const tl_object_ptr<td_api::ChatMemberStatus> &chat_member_status,
                                                    Promise<Unit> &&promise) {
  auto status = get_dialog_participant_status(chat_member_status);
  LOG(INFO) << "Receive SetChatMemberStatus request with " << user_id << " and " << dialog_id << " to " << status;
  if (!have_dialog_force(dialog_id)) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(3, "Chat member status can't be changed in private chats"));
    case DialogType::Chat:
      return td_->contacts_manager_->change_chat_participant_status(dialog_id.get_chat_id(), user_id, status,
                                                                    std::move(promise));
    case DialogType::Channel:
      return td_->contacts_manager_->change_channel_participant_status(dialog_id.get_channel_id(), user_id, status,
                                                                       std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(3, "Chat member status can't be changed in secret chats"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

DialogParticipant MessagesManager::get_dialog_participant(DialogId dialog_id, UserId user_id, int64 &random_id,
                                                          bool force, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive GetChatMember request to get " << user_id << " in " << dialog_id;
  if (!have_dialog_force(dialog_id)) {
    promise.set_error(Status::Error(3, "Chat not found"));
    return DialogParticipant();
  }

  switch (dialog_id.get_type()) {
    case DialogType::User: {
      auto peer_user_id = dialog_id.get_user_id();
      if (user_id == td_->contacts_manager_->get_my_id("get_dialog_participant")) {
        promise.set_value(Unit());
        return {user_id, peer_user_id, 0, DialogParticipantStatus::Member()};
      }
      if (user_id == peer_user_id) {
        promise.set_value(Unit());
        return {peer_user_id, user_id, 0, DialogParticipantStatus::Member()};
      }

      promise.set_error(Status::Error(3, "User is not a member of the private chat"));
      break;
    }
    case DialogType::Chat:
      return td_->contacts_manager_->get_chat_participant(dialog_id.get_chat_id(), user_id, force, std::move(promise));
    case DialogType::Channel:
      return td_->contacts_manager_->get_channel_participant(dialog_id.get_channel_id(), user_id, random_id, force,
                                                             std::move(promise));
    case DialogType::SecretChat: {
      auto peer_user_id = td_->contacts_manager_->get_secret_chat_user_id(dialog_id.get_secret_chat_id());
      if (user_id == td_->contacts_manager_->get_my_id("get_dialog_participant")) {
        promise.set_value(Unit());
        return {user_id, peer_user_id, 0, DialogParticipantStatus::Member()};
      }
      if (user_id == peer_user_id) {
        promise.set_value(Unit());
        return {peer_user_id, user_id, 0, DialogParticipantStatus::Member()};
      }

      promise.set_error(Status::Error(3, "User is not a member of the secret chat"));
      break;
    }
    case DialogType::None:
    default:
      UNREACHABLE();
      promise.set_error(Status::Error(500, "Wrong chat type"));
  }
  return DialogParticipant();
}

std::pair<int32, vector<DialogParticipant>> MessagesManager::search_private_chat_participants(UserId my_user_id,
                                                                                              UserId peer_user_id,
                                                                                              const string &query,
                                                                                              int32 limit) const {
  auto result = td_->contacts_manager_->search_among_users({my_user_id, peer_user_id}, query, limit);
  return {result.first, transform(result.second, [&](UserId user_id) {
            return DialogParticipant(user_id, user_id == my_user_id ? peer_user_id : my_user_id, 0,
                                     DialogParticipantStatus::Member());
          })};
}

std::pair<int32, vector<DialogParticipant>> MessagesManager::search_dialog_participants(
    DialogId dialog_id, const string &query, int32 limit, int64 &random_id, bool force, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive SearchChatMembers request to search for " << query << " in " << dialog_id;
  if (!have_dialog_force(dialog_id)) {
    promise.set_error(Status::Error(3, "Chat not found"));
    return {};
  }
  if (limit < 0) {
    promise.set_error(Status::Error(3, "Parameter limit must be non-negative"));
    return {};
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      promise.set_value(Unit());
      return search_private_chat_participants(td_->contacts_manager_->get_my_id("search_dialog_participants"),
                                              dialog_id.get_user_id(), query, limit);
    case DialogType::Chat:
      return td_->contacts_manager_->search_chat_participants(dialog_id.get_chat_id(), query, limit, force,
                                                              std::move(promise));
    case DialogType::Channel:
      return td_->contacts_manager_->get_channel_participants(
          dialog_id.get_channel_id(), td_api::make_object<td_api::supergroupMembersFilterSearch>(query), 0, limit,
          random_id, force, std::move(promise));
    case DialogType::SecretChat: {
      promise.set_value(Unit());
      auto peer_user_id = td_->contacts_manager_->get_secret_chat_user_id(dialog_id.get_secret_chat_id());
      return search_private_chat_participants(td_->contacts_manager_->get_my_id("search_dialog_participants"),
                                              peer_user_id, query, limit);
    }
    case DialogType::None:
    default:
      UNREACHABLE();
      promise.set_error(Status::Error(500, "Wrong chat type"));
  }
  return {};
}

vector<UserId> MessagesManager::get_dialog_administrators(DialogId dialog_id, int left_tries, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive GetChatAdministrators request in " << dialog_id;
  if (!have_dialog_force(dialog_id)) {
    promise.set_error(Status::Error(3, "Chat not found"));
    return {};
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::SecretChat:
      promise.set_value(Unit());
      break;
    case DialogType::Chat:
    case DialogType::Channel:
      return td_->contacts_manager_->get_dialog_administrators(dialog_id, left_tries, std::move(promise));
    case DialogType::None:
    default:
      UNREACHABLE();
      promise.set_error(Status::Error(500, "Wrong chat type"));
  }
  return {};
}

void MessagesManager::export_dialog_invite_link(DialogId dialog_id, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive ExportDialogInviteLink request for " << dialog_id;
  if (!have_dialog_force(dialog_id)) {
    return promise.set_error(Status::Error(3, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(3, "Can't invite members to a private chat"));
    case DialogType::Chat:
      return td_->contacts_manager_->export_chat_invite_link(dialog_id.get_chat_id(), std::move(promise));
    case DialogType::Channel:
      return td_->contacts_manager_->export_channel_invite_link(dialog_id.get_channel_id(), std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(3, "Can't invite members to a secret chat"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

string MessagesManager::get_dialog_invite_link(DialogId dialog_id) {
  switch (dialog_id.get_type()) {
    case DialogType::Chat:
      return td_->contacts_manager_->get_chat_invite_link(dialog_id.get_chat_id());
    case DialogType::Channel:
      return td_->contacts_manager_->get_channel_invite_link(dialog_id.get_channel_id());
    case DialogType::User:
    case DialogType::SecretChat:
    case DialogType::None:
      return string();
    default:
      UNREACHABLE();
      return string();
  }
}

tl_object_ptr<telegram_api::channelAdminLogEventsFilter> MessagesManager::get_channel_admin_log_events_filter(
    const tl_object_ptr<td_api::chatEventLogFilters> &filters) {
  if (filters == nullptr) {
    return nullptr;
  }

  int32 flags = 0;
  if (filters->message_edits_) {
    flags |= telegram_api::channelAdminLogEventsFilter::EDIT_MASK;
  }
  if (filters->message_deletions_) {
    flags |= telegram_api::channelAdminLogEventsFilter::DELETE_MASK;
  }
  if (filters->message_pins_) {
    flags |= telegram_api::channelAdminLogEventsFilter::PINNED_MASK;
  }
  if (filters->member_joins_) {
    flags |= telegram_api::channelAdminLogEventsFilter::JOIN_MASK;
  }
  if (filters->member_leaves_) {
    flags |= telegram_api::channelAdminLogEventsFilter::LEAVE_MASK;
  }
  if (filters->member_invites_) {
    flags |= telegram_api::channelAdminLogEventsFilter::INVITE_MASK;
  }
  if (filters->member_promotions_) {
    flags |= telegram_api::channelAdminLogEventsFilter::PROMOTE_MASK;
    flags |= telegram_api::channelAdminLogEventsFilter::DEMOTE_MASK;
  }
  if (filters->member_restrictions_) {
    flags |= telegram_api::channelAdminLogEventsFilter::BAN_MASK;
    flags |= telegram_api::channelAdminLogEventsFilter::UNBAN_MASK;
    flags |= telegram_api::channelAdminLogEventsFilter::KICK_MASK;
    flags |= telegram_api::channelAdminLogEventsFilter::UNKICK_MASK;
  }
  if (filters->info_changes_) {
    flags |= telegram_api::channelAdminLogEventsFilter::INFO_MASK;
  }
  if (filters->setting_changes_) {
    flags |= telegram_api::channelAdminLogEventsFilter::SETTINGS_MASK;
  }

  return make_tl_object<telegram_api::channelAdminLogEventsFilter>(
      flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
      false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
      false /*ignored*/, false /*ignored*/, false /*ignored*/);
}

int64 MessagesManager::get_dialog_event_log(DialogId dialog_id, const string &query, int64 from_event_id, int32 limit,
                                            const tl_object_ptr<td_api::chatEventLogFilters> &filters,
                                            const vector<UserId> &user_ids, Promise<Unit> &&promise) {
  if (td_->auth_manager_->is_bot()) {
    promise.set_error(Status::Error(3, "Method is not available for bots"));
    return 0;
  }

  if (!have_dialog_force(dialog_id)) {
    promise.set_error(Status::Error(3, "Chat not found"));
    return 0;
  }

  if (dialog_id.get_type() != DialogType::Channel) {
    promise.set_error(Status::Error(3, "Chat is not a supergroup chat"));
    return 0;
  }

  auto channel_id = dialog_id.get_channel_id();
  if (!td_->contacts_manager_->have_channel(channel_id)) {
    promise.set_error(Status::Error(3, "Chat info not found"));
    return 0;
  }

  if (!td_->contacts_manager_->get_channel_status(channel_id).is_administrator()) {
    promise.set_error(Status::Error(3, "Not enough rights to get event log"));
    return 0;
  }

  vector<tl_object_ptr<telegram_api::InputUser>> input_users;
  for (auto user_id : user_ids) {
    auto input_user = td_->contacts_manager_->get_input_user(user_id);
    if (input_user == nullptr) {
      promise.set_error(Status::Error(3, "User not found"));
      return 0;
    }
    input_users.push_back(std::move(input_user));
  }

  int64 random_id = 0;
  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || chat_events_.find(random_id) != chat_events_.end());
  chat_events_[random_id];  // reserve place for result

  td_->create_handler<GetChannelAdminLogQuery>(std::move(promise))
      ->send(channel_id, query, from_event_id, limit, get_channel_admin_log_events_filter(filters),
             std::move(input_users), random_id);

  return random_id;
}

tl_object_ptr<td_api::ChatEventAction> MessagesManager::get_chat_event_action_object(
    tl_object_ptr<telegram_api::ChannelAdminLogEventAction> &&action_ptr) {
  CHECK(action_ptr != nullptr);
  switch (action_ptr->get_id()) {
    case telegram_api::channelAdminLogEventActionParticipantJoin::ID:
      return make_tl_object<td_api::chatEventMemberJoined>();
    case telegram_api::channelAdminLogEventActionParticipantLeave::ID:
      return make_tl_object<td_api::chatEventMemberLeft>();
    case telegram_api::channelAdminLogEventActionParticipantInvite::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionParticipantInvite>(action_ptr);
      auto member = td_->contacts_manager_->get_dialog_participant(ChannelId(), std::move(action->participant_));
      return make_tl_object<td_api::chatEventMemberInvited>(
          td_->contacts_manager_->get_user_id_object(member.user_id, "chatEventMemberInvited"),
          member.status.get_chat_member_status_object());
    }
    case telegram_api::channelAdminLogEventActionParticipantToggleBan::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionParticipantToggleBan>(action_ptr);
      auto old_member =
          td_->contacts_manager_->get_dialog_participant(ChannelId(), std::move(action->prev_participant_));
      auto new_member =
          td_->contacts_manager_->get_dialog_participant(ChannelId(), std::move(action->new_participant_));
      if (old_member.user_id != new_member.user_id) {
        LOG(ERROR) << old_member.user_id << " VS " << new_member.user_id;
        return nullptr;
      }
      return make_tl_object<td_api::chatEventMemberRestricted>(
          td_->contacts_manager_->get_user_id_object(old_member.user_id, "chatEventMemberRestricted"),
          old_member.status.get_chat_member_status_object(), new_member.status.get_chat_member_status_object());
    }
    case telegram_api::channelAdminLogEventActionParticipantToggleAdmin::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionParticipantToggleAdmin>(action_ptr);
      auto old_member =
          td_->contacts_manager_->get_dialog_participant(ChannelId(), std::move(action->prev_participant_));
      auto new_member =
          td_->contacts_manager_->get_dialog_participant(ChannelId(), std::move(action->new_participant_));
      if (old_member.user_id != new_member.user_id) {
        LOG(ERROR) << old_member.user_id << " VS " << new_member.user_id;
        return nullptr;
      }
      return make_tl_object<td_api::chatEventMemberPromoted>(
          td_->contacts_manager_->get_user_id_object(old_member.user_id, "chatEventMemberPromoted"),
          old_member.status.get_chat_member_status_object(), new_member.status.get_chat_member_status_object());
    }
    case telegram_api::channelAdminLogEventActionChangeTitle::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionChangeTitle>(action_ptr);
      return make_tl_object<td_api::chatEventTitleChanged>(std::move(action->prev_value_),
                                                           std::move(action->new_value_));
    }
    case telegram_api::channelAdminLogEventActionChangeAbout::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionChangeAbout>(action_ptr);
      return make_tl_object<td_api::chatEventDescriptionChanged>(std::move(action->prev_value_),
                                                                 std::move(action->new_value_));
    }
    case telegram_api::channelAdminLogEventActionChangeUsername::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionChangeUsername>(action_ptr);
      return make_tl_object<td_api::chatEventUsernameChanged>(std::move(action->prev_value_),
                                                              std::move(action->new_value_));
    }
    case telegram_api::channelAdminLogEventActionChangePhoto::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionChangePhoto>(action_ptr);
      auto file_manager = td_->file_manager_.get();
      auto old_photo = td::get_dialog_photo(file_manager, std::move(action->prev_photo_));
      auto new_photo = td::get_dialog_photo(file_manager, std::move(action->new_photo_));
      return make_tl_object<td_api::chatEventPhotoChanged>(get_chat_photo_object(file_manager, &old_photo),
                                                           get_chat_photo_object(file_manager, &new_photo));
    }
    case telegram_api::channelAdminLogEventActionToggleInvites::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionToggleInvites>(action_ptr);
      return make_tl_object<td_api::chatEventInvitesToggled>(action->new_value_);
    }
    case telegram_api::channelAdminLogEventActionToggleSignatures::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionToggleSignatures>(action_ptr);
      return make_tl_object<td_api::chatEventSignMessagesToggled>(action->new_value_);
    }
    case telegram_api::channelAdminLogEventActionUpdatePinned::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionUpdatePinned>(action_ptr);
      auto message = create_message(
          parse_telegram_api_message(std::move(action->message_), "channelAdminLogEventActionUpdatePinned"), true);
      if (message.second == nullptr) {
        return make_tl_object<td_api::chatEventMessageUnpinned>();
      }
      return make_tl_object<td_api::chatEventMessagePinned>(get_message_object(message.first, message.second.get()));
    }
    case telegram_api::channelAdminLogEventActionEditMessage::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionEditMessage>(action_ptr);
      auto old_message = create_message(
          parse_telegram_api_message(std::move(action->prev_message_), "prev channelAdminLogEventActionEditMessage"),
          true);
      auto new_message = create_message(
          parse_telegram_api_message(std::move(action->new_message_), "new channelAdminLogEventActionEditMessage"),
          true);
      if (old_message.second == nullptr || new_message.second == nullptr || old_message.first != new_message.first) {
        LOG(ERROR) << "Failed to get edited message";
        return nullptr;
      }
      return make_tl_object<td_api::chatEventMessageEdited>(
          get_message_object(old_message.first, old_message.second.get()),
          get_message_object(new_message.first, new_message.second.get()));
    }
    case telegram_api::channelAdminLogEventActionDeleteMessage::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionDeleteMessage>(action_ptr);
      auto message = create_message(
          parse_telegram_api_message(std::move(action->message_), "channelAdminLogEventActionDeleteMessage"), true);
      if (message.second == nullptr) {
        LOG(ERROR) << "Failed to get deleted message";
        return nullptr;
      }
      return make_tl_object<td_api::chatEventMessageDeleted>(get_message_object(message.first, message.second.get()));
    }
    case telegram_api::channelAdminLogEventActionChangeStickerSet::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionChangeStickerSet>(action_ptr);
      auto old_sticker_set_id = td_->stickers_manager_->add_sticker_set(std::move(action->prev_stickerset_));
      auto new_sticker_set_id = td_->stickers_manager_->add_sticker_set(std::move(action->new_stickerset_));
      return make_tl_object<td_api::chatEventStickerSetChanged>(old_sticker_set_id, new_sticker_set_id);
    }
    case telegram_api::channelAdminLogEventActionTogglePreHistoryHidden::ID: {
      auto action = move_tl_object_as<telegram_api::channelAdminLogEventActionTogglePreHistoryHidden>(action_ptr);
      return make_tl_object<td_api::chatEventIsAllHistoryAvailableToggled>(!action->new_value_);
    }
    default:
      UNREACHABLE();
      return nullptr;
  }
}

void MessagesManager::on_get_event_log(int64 random_id,
                                       tl_object_ptr<telegram_api::channels_adminLogResults> &&events) {
  auto it = chat_events_.find(random_id);
  CHECK(it != chat_events_.end());
  auto &result = it->second;
  CHECK(result == nullptr);

  if (events == nullptr) {
    chat_events_.erase(it);
    return;
  }

  LOG(INFO) << "Receive " << to_string(events);

  td_->contacts_manager_->on_get_users(std::move(events->users_));
  td_->contacts_manager_->on_get_chats(std::move(events->chats_));

  result = make_tl_object<td_api::chatEvents>();
  result->events_.reserve(events->events_.size());
  for (auto &event : events->events_) {
    if (event->date_ <= 0) {
      LOG(ERROR) << "Receive wrong event date = " << event->date_;
      event->date_ = 0;
    }

    UserId user_id(event->user_id_);
    if (!user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid " << user_id;
      continue;
    }
    LOG_IF(ERROR, !td_->contacts_manager_->have_user(user_id)) << "Have no info about " << user_id;

    auto action = get_chat_event_action_object(std::move(event->action_));
    if (action == nullptr) {
      continue;
    }
    result->events_.push_back(make_tl_object<td_api::chatEvent>(
        event->id_, event->date_, td_->contacts_manager_->get_user_id_object(user_id, "chatEvent"), std::move(action)));
  }
}

tl_object_ptr<td_api::chatEvents> MessagesManager::get_chat_events_object(int64 random_id) {
  auto it = chat_events_.find(random_id);
  CHECK(it != chat_events_.end());
  auto result = std::move(it->second);
  chat_events_.erase(it);
  return result;
}

unique_ptr<MessagesManager::Message> *MessagesManager::find_message(unique_ptr<Message> *v, MessageId message_id) {
  return const_cast<unique_ptr<Message> *>(find_message(static_cast<const unique_ptr<Message> *>(v), message_id));
}

const unique_ptr<MessagesManager::Message> *MessagesManager::find_message(const unique_ptr<Message> *v,
                                                                          MessageId message_id) {
  LOG(DEBUG) << "Searching for " << message_id << " in " << static_cast<const void *>(v->get());
  while (*v != nullptr) {
    //    LOG(DEBUG) << "Pass " << (*v)->message_id;
    if ((*v)->message_id.get() < message_id.get()) {
      //      LOG(DEBUG) << "Go right";
      v = &(*v)->right;
    } else if ((*v)->message_id.get() > message_id.get()) {
      //      LOG(DEBUG) << "Go left";
      v = &(*v)->left;
    } else {
      LOG(DEBUG) << "Message found";
      break;
    }
  }
  return v;
}

MessagesManager::Message *MessagesManager::get_message(Dialog *d, MessageId message_id) {
  return const_cast<Message *>(get_message(static_cast<const Dialog *>(d), message_id));
}

const MessagesManager::Message *MessagesManager::get_message(const Dialog *d, MessageId message_id) {
  if (!message_id.is_valid()) {
    return nullptr;
  }

  CHECK(d != nullptr);
  LOG(DEBUG) << "Search for " << message_id << " in " << d->dialog_id;
  auto result = find_message(&d->messages, message_id)->get();
  if (result != nullptr) {
    result->last_access_date = G()->unix_time_cached();
  }
  return result;
}

MessagesManager::Message *MessagesManager::get_message_force(Dialog *d, MessageId message_id) {
  if (!message_id.is_valid()) {
    return nullptr;
  }

  auto result = get_message(d, message_id);
  if (result != nullptr) {
    return result;
  }

  if (!G()->parameters().use_message_db || message_id.is_yet_unsent()) {
    return nullptr;
  }

  if (d->deleted_message_ids.count(message_id)) {
    return nullptr;
  }

  LOG(INFO) << "Try to load " << FullMessageId{d->dialog_id, message_id} << " from database";
  auto r_value = G()->td_db()->get_messages_db_sync()->get_message({d->dialog_id, message_id});
  if (r_value.is_error()) {
    return nullptr;
  }
  return on_get_message_from_database(d->dialog_id, d, r_value.ok());
}

MessagesManager::Message *MessagesManager::on_get_message_from_database(DialogId dialog_id, Dialog *d,
                                                                        const BufferSlice &value) {
  if (value.empty()) {
    return nullptr;
  }

  auto m = make_unique<Message>();
  log_event_parse(*m, value.as_slice()).ensure();

  if (d == nullptr) {
    LOG(ERROR) << "Can't find " << dialog_id << ", but have a message from it";
    if (!dialog_id.is_valid()) {
      LOG(ERROR) << "Got message in invalid " << dialog_id;
      return nullptr;
    }

    get_messages_from_server({FullMessageId{dialog_id, m->message_id}}, Auto());

    force_create_dialog(dialog_id, "on_get_message_from_database");
    d = get_dialog_force(dialog_id);
    CHECK(d != nullptr);
  }

  if (!have_input_peer(d->dialog_id, AccessRights::Read)) {
    return nullptr;
  }

  auto old_message = get_message(d, m->message_id);
  if (old_message != nullptr) {
    // data in the database is always outdated, so return a message from the memory
    return old_message;
  }

  Dependencies dependencies;
  add_message_dependencies(dependencies, d->dialog_id, m.get());
  resolve_dependencies_force(dependencies);

  m->have_next = false;
  m->have_previous = false;
  m->from_database = true;
  bool need_update = false;
  bool need_update_dialog_pos = false;
  auto result = add_message_to_dialog(d, std::move(m), false, &need_update, &need_update_dialog_pos,
                                      "on_get_message_from_database");
  if (need_update_dialog_pos) {
    LOG(ERROR) << "Need update dialog pos after load " << (result == nullptr ? MessageId() : result->message_id)
               << " in " << d->dialog_id << " from database";
    send_update_chat_last_message(d, "on_get_message_from_database");
  }
  return result;
}

NotificationSettings MessagesManager::get_notification_settings(
    tl_object_ptr<telegram_api::PeerNotifySettings> &&notification_settings) {
  int32 constructor_id = notification_settings->get_id();
  if (constructor_id == telegram_api::peerNotifySettingsEmpty::ID) {
    LOG(ERROR) << "Empty notify settings received";
    return {};
  }
  CHECK(constructor_id == telegram_api::peerNotifySettings::ID);
  auto settings = static_cast<const telegram_api::peerNotifySettings *>(notification_settings.get());
  auto mute_until = (settings->mute_until_ <= G()->unix_time() ? 0 : settings->mute_until_);
  return {mute_until, settings->sound_, (settings->flags_ & telegram_api::peerNotifySettings::SHOW_PREVIEWS_MASK) != 0,
          (settings->flags_ & telegram_api::peerNotifySettings::SILENT_MASK) != 0};
}

unique_ptr<MessageContent> MessagesManager::get_secret_message_document(
    tl_object_ptr<telegram_api::encryptedFile> file,
    tl_object_ptr<secret_api::decryptedMessageMediaDocument> &&document,
    vector<tl_object_ptr<telegram_api::DocumentAttribute>> &&attributes, DialogId owner_dialog_id,
    FormattedText &&caption, bool is_opened) const {
  return get_message_document(td_->documents_manager_->on_get_document(
                                  {std::move(file), std::move(document), std::move(attributes)}, owner_dialog_id),
                              std::move(caption), is_opened);
}

unique_ptr<MessageContent> MessagesManager::get_message_document(tl_object_ptr<telegram_api::document> &&document,
                                                                 DialogId owner_dialog_id, FormattedText &&caption,
                                                                 bool is_opened,
                                                                 MultiPromiseActor *load_data_multipromise_ptr) const {
  return get_message_document(
      td_->documents_manager_->on_get_document(std::move(document), owner_dialog_id, load_data_multipromise_ptr),
      std::move(caption), is_opened);
}

unique_ptr<MessageContent> MessagesManager::get_message_document(
    std::pair<DocumentsManager::DocumentType, FileId> &&parsed_document, FormattedText &&caption,
    bool is_opened) const {
  auto document_type = parsed_document.first;
  auto file_id = parsed_document.second;
  if (document_type != DocumentsManager::DocumentType::Unknown) {
    CHECK(file_id.is_valid());
  }
  switch (document_type) {
    case DocumentsManager::DocumentType::Animation:
      return make_unique<MessageAnimation>(file_id, std::move(caption));
    case DocumentsManager::DocumentType::Audio:
      return make_unique<MessageAudio>(file_id, std::move(caption));
    case DocumentsManager::DocumentType::General:
      return make_unique<MessageDocument>(file_id, std::move(caption));
    case DocumentsManager::DocumentType::Sticker:
      return make_unique<MessageSticker>(file_id);
    case DocumentsManager::DocumentType::Unknown:
      return make_unique<MessageUnsupported>();
    case DocumentsManager::DocumentType::Video:
      return make_unique<MessageVideo>(file_id, std::move(caption));
    case DocumentsManager::DocumentType::VideoNote:
      return make_unique<MessageVideoNote>(file_id, is_opened);
    case DocumentsManager::DocumentType::VoiceNote:
      return make_unique<MessageVoiceNote>(file_id, std::move(caption), is_opened);
    default:
      UNREACHABLE();
      return nullptr;
  }
}

unique_ptr<MessagePhoto> MessagesManager::get_message_photo(tl_object_ptr<telegram_api::photo> &&photo,
                                                            DialogId owner_dialog_id, FormattedText &&caption) const {
  auto m = make_unique<MessagePhoto>();

  m->photo = get_photo(td_->file_manager_.get(), std::move(photo), owner_dialog_id);
  m->caption = std::move(caption);

  return m;
}

FormattedText MessagesManager::get_secret_media_caption(string &&message_text, string &&message_caption) {
  FormattedText caption;
  if (message_text.empty()) {
    caption.text = std::move(message_caption);
  } else if (message_caption.empty()) {
    caption.text = std::move(message_text);
  } else {
    caption.text = message_text + "\n\n" + message_caption;
  }

  caption.entities = find_entities(caption.text, false);
  return caption;
}

FormattedText MessagesManager::get_message_text(string message_text,
                                                vector<tl_object_ptr<telegram_api::MessageEntity>> &&server_entities,
                                                int32 send_date) const {
  auto entities = get_message_entities(td_->contacts_manager_.get(), std::move(server_entities), "get_message_text");
  auto status = fix_formatted_text(message_text, entities, true, true, true, false);
  if (status.is_error()) {
    if (send_date == 0 || send_date > 1497000000) {  // approximate fix date
      LOG(ERROR) << "Receive error " << status << " while parsing message content \"" << message_text << "\" sent at "
                 << send_date << " with entities " << format::as_array(entities);
    }
    if (!clean_input_string(message_text)) {
      message_text.clear();
    }
    entities.clear();
  }
  return FormattedText{std::move(message_text), std::move(entities)};
}

template <class ToT, class FromT>
static tl_object_ptr<ToT> secret_to_telegram(FromT &from);

// fileLocationUnavailable#7c596b46 volume_id:long local_id:int secret:long = FileLocation;
static auto secret_to_telegram(secret_api::fileLocationUnavailable &file_location) {
  return make_tl_object<telegram_api::fileLocationUnavailable>(file_location.volume_id_, file_location.local_id_,
                                                               file_location.secret_);
}

// fileLocation#53d69076 dc_id:int volume_id:long local_id:int secret:long = FileLocation;
static auto secret_to_telegram(secret_api::fileLocation &file_location) {
  return make_tl_object<telegram_api::fileLocation>(file_location.dc_id_, file_location.volume_id_,
                                                    file_location.local_id_, file_location.secret_);
}

// photoSizeEmpty#e17e23c type:string = PhotoSize;
static auto secret_to_telegram(secret_api::photoSizeEmpty &empty) {
  if (!clean_input_string(empty.type_)) {
    empty.type_.clear();
  }
  return make_tl_object<telegram_api::photoSizeEmpty>(empty.type_);
}

// photoSize#77bfb61b type:string location:FileLocation w:int h:int size:int = PhotoSize;
static auto secret_to_telegram(secret_api::photoSize &photo_size) {
  if (!clean_input_string(photo_size.type_)) {
    photo_size.type_.clear();
  }
  return make_tl_object<telegram_api::photoSize>(photo_size.type_,
                                                 secret_to_telegram<telegram_api::FileLocation>(*photo_size.location_),
                                                 photo_size.w_, photo_size.h_, photo_size.size_);
}

// photoCachedSize#e9a734fa type:string location:FileLocation w:int h:int bytes:bytes = PhotoSize;
static auto secret_to_telegram(secret_api::photoCachedSize &photo_size) {
  if (!clean_input_string(photo_size.type_)) {
    photo_size.type_.clear();
  }
  return make_tl_object<telegram_api::photoCachedSize>(
      photo_size.type_, secret_to_telegram<telegram_api::FileLocation>(*photo_size.location_), photo_size.w_,
      photo_size.h_, photo_size.bytes_.clone());
}

// documentAttributeImageSize #6c37c15c w:int h:int = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeImageSize &image_size) {
  return make_tl_object<telegram_api::documentAttributeImageSize>(image_size.w_, image_size.h_);
}

// documentAttributeAnimated #11b58939 = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeAnimated &animated) {
  return make_tl_object<telegram_api::documentAttributeAnimated>();
}

// documentAttributeSticker23 #fb0a5727 = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeSticker23 &sticker) {
  return make_tl_object<telegram_api::documentAttributeSticker>(
      0, false /*ignored*/, "", make_tl_object<telegram_api::inputStickerSetEmpty>(), nullptr);
}
static auto secret_to_telegram(secret_api::inputStickerSetEmpty &sticker_set) {
  return make_tl_object<telegram_api::inputStickerSetEmpty>();
}
static auto secret_to_telegram(secret_api::inputStickerSetShortName &sticker_set) {
  if (!clean_input_string(sticker_set.short_name_)) {
    sticker_set.short_name_.clear();
  }
  return make_tl_object<telegram_api::inputStickerSetShortName>(sticker_set.short_name_);
}

// documentAttributeSticker #3a556302 alt:string stickerset:InputStickerSet = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeSticker &sticker) {
  if (!clean_input_string(sticker.alt_)) {
    sticker.alt_.clear();
  }
  return make_tl_object<telegram_api::documentAttributeSticker>(
      0, false /*ignored*/, sticker.alt_, secret_to_telegram<telegram_api::InputStickerSet>(*sticker.stickerset_),
      nullptr);
}

// documentAttributeVideo #5910cccb duration:int w:int h:int = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeVideo &video) {
  return make_tl_object<telegram_api::documentAttributeVideo>(0, false /*ignored*/, false /*ignored*/, video.duration_,
                                                              video.w_, video.h_);
}

// documentAttributeFilename #15590068 file_name:string = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeFilename &filename) {
  if (!clean_input_string(filename.file_name_)) {
    filename.file_name_.clear();
  }
  return make_tl_object<telegram_api::documentAttributeFilename>(filename.file_name_);
}

// documentAttributeVideo66#ef02ce6 flags:# round_message:flags.0?true duration:int w:int h:int = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeVideo66 &video) {
  return make_tl_object<telegram_api::documentAttributeVideo>(
      (video.flags_ & secret_api::documentAttributeVideo66::ROUND_MESSAGE_MASK) != 0
          ? telegram_api::documentAttributeVideo::ROUND_MESSAGE_MASK
          : 0,
      video.round_message_, false, video.duration_, video.w_, video.h_);
}

static auto telegram_documentAttributeAudio(bool is_voice_note, int duration, string title, string performer,
                                            BufferSlice waveform) {
  if (!clean_input_string(title)) {
    title.clear();
  }
  if (!clean_input_string(performer)) {
    performer.clear();
  }

  int32 flags = 0;
  if (is_voice_note) {
    flags |= telegram_api::documentAttributeAudio::VOICE_MASK;
  }
  if (!title.empty()) {
    flags |= telegram_api::documentAttributeAudio::TITLE_MASK;
  }
  if (!performer.empty()) {
    flags |= telegram_api::documentAttributeAudio::PERFORMER_MASK;
  }
  if (waveform.size()) {
    flags |= telegram_api::documentAttributeAudio::WAVEFORM_MASK;
  }
  return make_tl_object<telegram_api::documentAttributeAudio>(flags, is_voice_note, duration, std::move(title),
                                                              std::move(performer), std::move(waveform));
}

// documentAttributeAudio23 #51448e5 duration:int = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeAudio23 &audio) {
  return telegram_documentAttributeAudio(false, audio.duration_, "", "", Auto());
}
// documentAttributeAudio45 #ded218e0 duration:int title:string performer:string = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeAudio45 &audio) {
  return telegram_documentAttributeAudio(false, audio.duration_, audio.title_, audio.performer_, Auto());
}

// documentAttributeAudio#9852f9c6 flags:# voice:flags.10?true duration:int title:flags.0?string
//    performer:flags.1?string waveform:flags.2?bytes = DocumentAttribute;
static auto secret_to_telegram(secret_api::documentAttributeAudio &audio) {
  return telegram_documentAttributeAudio((audio.flags_ & secret_api::documentAttributeAudio::VOICE_MASK) != 0,
                                         audio.duration_, audio.title_, audio.performer_, audio.waveform_.clone());
}

static auto secret_to_telegram(std::vector<tl_object_ptr<secret_api::DocumentAttribute>> &attributes) {
  std::vector<tl_object_ptr<telegram_api::DocumentAttribute>> res;
  for (auto &attribute : attributes) {
    auto telegram_attribute = secret_to_telegram<telegram_api::DocumentAttribute>(*attribute);
    if (telegram_attribute) {
      res.push_back(std::move(telegram_attribute));
    }
  }
  return res;
}

// decryptedMessageMediaExternalDocument#fa95b0dd id:long access_hash:long date:int mime_type:string size:int
// thumb:PhotoSize dc_id:int attributes:Vector<DocumentAttribute> = DecryptedMessageMedia;
static auto secret_to_telegram_document(secret_api::decryptedMessageMediaExternalDocument &from) {
  if (!clean_input_string(from.mime_type_)) {
    from.mime_type_.clear();
  }
  return make_tl_object<telegram_api::document>(from.id_, from.access_hash_, from.date_, from.mime_type_, from.size_,
                                                secret_to_telegram<telegram_api::PhotoSize>(*from.thumb_), from.dc_id_,
                                                0, secret_to_telegram(from.attributes_));
}

template <class ToT, class FromT>
static tl_object_ptr<ToT> secret_to_telegram(FromT &from) {
  tl_object_ptr<ToT> res;
  downcast_call(from, [&](auto &p) { res = secret_to_telegram(p); });
  return res;
}

Photo MessagesManager::get_web_document_photo(tl_object_ptr<telegram_api::WebDocument> web_document,
                                              DialogId owner_dialog_id) const {
  PhotoSize s =
      get_web_document_photo_size(td_->file_manager_.get(), FileType::Photo, owner_dialog_id, std::move(web_document));
  Photo photo;
  if (!s.file_id.is_valid()) {
    photo.id = -2;
  } else {
    photo.id = 0;
    photo.photos.push_back(s);
  }
  return photo;
}

unique_ptr<MessageContent> MessagesManager::get_secret_message_content(
    string message_text, tl_object_ptr<telegram_api::encryptedFile> file,
    tl_object_ptr<secret_api::DecryptedMessageMedia> &&media,
    vector<tl_object_ptr<secret_api::MessageEntity>> &&secret_entities, DialogId owner_dialog_id,
    MultiPromiseActor &load_data_multipromise) const {
  auto entities = get_message_entities(std::move(secret_entities));
  auto status = fix_formatted_text(message_text, entities, true, false, true, false);
  if (status.is_error()) {
    LOG(WARNING) << "Receive error " << status << " while parsing secret message \"" << message_text
                 << "\" with entities " << format::as_array(entities);
    if (!clean_input_string(message_text)) {
      message_text.clear();
    }
    entities.clear();
  }

  if (media == nullptr) {
    return make_unique<MessageText>(FormattedText{std::move(message_text), std::move(entities)}, WebPageId());
  }

  int32 constructor_id = media->get_id();
  if (message_text.size()) {
    if (constructor_id != secret_api::decryptedMessageMediaEmpty::ID) {
      LOG(INFO) << "Receive non-empty message text and media";
    } else {
      return make_unique<MessageText>(FormattedText{std::move(message_text), std::move(entities)}, WebPageId());
    }
  }

  // support of old layer and old constructions
  switch (constructor_id) {
    case secret_api::decryptedMessageMediaVideo::ID: {
      auto video = move_tl_object_as<secret_api::decryptedMessageMediaVideo>(media);
      std::vector<tl_object_ptr<secret_api::DocumentAttribute>> attributes;
      attributes.emplace_back(
          make_tl_object<secret_api::documentAttributeVideo>(video->duration_, video->w_, video->h_));
      media = make_tl_object<secret_api::decryptedMessageMediaDocument>(
          std::move(video->thumb_), video->thumb_w_, video->thumb_h_, video->mime_type_, video->size_,
          std::move(video->key_), std::move(video->iv_), std::move(attributes), std::move(video->caption_));

      constructor_id = secret_api::decryptedMessageMediaDocument::ID;
      break;
    }
  }

  bool is_media_empty = false;
  switch (constructor_id) {
    case secret_api::decryptedMessageMediaEmpty::ID:
      LOG(ERROR) << "Receive empty message text and media";
      is_media_empty = true;
      break;
    case secret_api::decryptedMessageMediaGeoPoint::ID: {
      auto message_geo_point = move_tl_object_as<secret_api::decryptedMessageMediaGeoPoint>(media);

      auto m = make_unique<MessageLocation>(Location(std::move(message_geo_point)));
      if (m->location.empty()) {
        is_media_empty = true;
        break;
      }

      return std::move(m);
    }
    case secret_api::decryptedMessageMediaVenue::ID: {
      auto message_venue = move_tl_object_as<secret_api::decryptedMessageMediaVenue>(media);

      if (!clean_input_string(message_venue->title_)) {
        message_venue->title_.clear();
      }
      if (!clean_input_string(message_venue->address_)) {
        message_venue->address_.clear();
      }
      if (!clean_input_string(message_venue->provider_)) {
        message_venue->provider_.clear();
      }
      if (!clean_input_string(message_venue->venue_id_)) {
        message_venue->venue_id_.clear();
      }

      auto m =
          make_unique<MessageVenue>(Venue(Location(message_venue->lat_, message_venue->long_),
                                          std::move(message_venue->title_), std::move(message_venue->address_),
                                          std::move(message_venue->provider_), std::move(message_venue->venue_id_)));
      if (m->venue.empty()) {
        is_media_empty = true;
        break;
      }

      return std::move(m);
    }
    case secret_api::decryptedMessageMediaContact::ID: {
      auto message_contact = move_tl_object_as<secret_api::decryptedMessageMediaContact>(media);
      if (!clean_input_string(message_contact->phone_number_)) {
        message_contact->phone_number_.clear();
      }
      if (!clean_input_string(message_contact->first_name_)) {
        message_contact->first_name_.clear();
      }
      if (!clean_input_string(message_contact->last_name_)) {
        message_contact->last_name_.clear();
      }
      return make_unique<MessageContact>(Contact(message_contact->phone_number_, message_contact->first_name_,
                                                 message_contact->last_name_, message_contact->user_id_));
    }
    case secret_api::decryptedMessageMediaWebPage::ID: {
      auto media_web_page = move_tl_object_as<secret_api::decryptedMessageMediaWebPage>(media);
      if (!clean_input_string(media_web_page->url_)) {
        media_web_page->url_.clear();
      }
      auto r_http_url = parse_url(media_web_page->url_);
      if (r_http_url.is_error()) {
        is_media_empty = true;
        break;
      }
      auto url = r_http_url.ok().get_url();

      auto web_page_id = td_->web_pages_manager_->get_web_page_by_url(url, load_data_multipromise.get_promise());
      auto result = make_unique<MessageText>(FormattedText{std::move(message_text), std::move(entities)}, web_page_id);
      if (!result->web_page_id.is_valid()) {
        load_data_multipromise.add_promise(
            PromiseCreator::lambda([td = td_, url, &web_page_id = result->web_page_id](Result<Unit> result) {
              if (result.is_ok()) {
                web_page_id = td->web_pages_manager_->get_web_page_by_url(url);
              }
            }));
      }
      return std::move(result);
    }
    case secret_api::decryptedMessageMediaExternalDocument::ID: {
      auto external_document = move_tl_object_as<secret_api::decryptedMessageMediaExternalDocument>(media);
      auto document = secret_to_telegram_document(*external_document);
      return get_message_document(std::move(document), owner_dialog_id,
                                  FormattedText{std::move(message_text), std::move(entities)}, false,
                                  &load_data_multipromise);
    }
  }
  if (file == nullptr && !is_media_empty) {
    LOG(ERROR) << "Received secret message with media, but without a file";
    is_media_empty = true;
  }
  if (is_media_empty) {
    return make_unique<MessageText>(FormattedText{std::move(message_text), std::move(entities)}, WebPageId());
  }
  switch (constructor_id) {
    case secret_api::decryptedMessageMediaPhoto::ID: {
      auto message_photo = move_tl_object_as<secret_api::decryptedMessageMediaPhoto>(media);
      if (!clean_input_string(message_photo->caption_)) {
        message_photo->caption_.clear();
      }
      return make_unique<MessagePhoto>(
          get_photo(td_->file_manager_.get(), std::move(file), std::move(message_photo), owner_dialog_id),
          get_secret_media_caption(std::move(message_text), std::move(message_photo->caption_)));
    }
    case secret_api::decryptedMessageMediaDocument::ID: {
      auto message_document = move_tl_object_as<secret_api::decryptedMessageMediaDocument>(media);
      if (!clean_input_string(message_document->caption_)) {
        message_document->caption_.clear();
      }
      if (!clean_input_string(message_document->mime_type_)) {
        message_document->mime_type_.clear();
      }
      auto attributes = secret_to_telegram(message_document->attributes_);
      message_document->attributes_.clear();
      return get_secret_message_document(
          std::move(file), std::move(message_document), std::move(attributes), owner_dialog_id,
          get_secret_media_caption(std::move(message_text), std::move(message_document->caption_)), false);
    }
    default:
      LOG(ERROR) << "Unsupported: " << to_string(media);
      return make_unique<MessageUnsupported>();
  }
}

unique_ptr<MessageContent> MessagesManager::get_message_content(FormattedText message,
                                                                tl_object_ptr<telegram_api::MessageMedia> &&media,
                                                                DialogId owner_dialog_id, bool is_content_read,
                                                                UserId via_bot_user_id, int32 *ttl) const {
  if (media == nullptr) {
    return make_unique<MessageText>(std::move(message), WebPageId());
  }

  int32 constructor_id = media->get_id();
  if (message.text.size()) {
    if (constructor_id != telegram_api::messageMediaEmpty::ID) {
      LOG(INFO) << "Receive non-empty message text and media for message from " << owner_dialog_id;
    } else {
      return make_unique<MessageText>(std::move(message), WebPageId());
    }
  }
  switch (constructor_id) {
    case telegram_api::messageMediaEmpty::ID:
      LOG(ERROR) << "Receive empty message text and media for message from " << owner_dialog_id;
      return make_unique<MessageText>(std::move(message), WebPageId());
    case telegram_api::messageMediaPhoto::ID: {
      auto message_photo = move_tl_object_as<telegram_api::messageMediaPhoto>(media);
      if ((message_photo->flags_ & telegram_api::messageMediaPhoto::PHOTO_MASK) == 0) {
        if ((message_photo->flags_ & telegram_api::messageMediaPhoto::TTL_SECONDS_MASK) == 0) {
          LOG(ERROR) << "Receive messageMediaPhoto without photo and TTL: " << oneline(to_string(message_photo));
          break;
        }

        return make_unique<MessageExpiredPhoto>();
      }

      auto photo_ptr = std::move(message_photo->photo_);
      int32 photo_id = photo_ptr->get_id();
      if (photo_id == telegram_api::photoEmpty::ID) {
        return make_unique<MessageExpiredPhoto>();
      }
      CHECK(photo_id == telegram_api::photo::ID);

      if (ttl != nullptr && (message_photo->flags_ & telegram_api::messageMediaPhoto::TTL_SECONDS_MASK) != 0) {
        *ttl = message_photo->ttl_seconds_;
      }
      return get_message_photo(move_tl_object_as<telegram_api::photo>(photo_ptr), owner_dialog_id, std::move(message));
    }
    case telegram_api::messageMediaGeo::ID: {
      auto message_geo_point = move_tl_object_as<telegram_api::messageMediaGeo>(media);

      auto m = make_unique<MessageLocation>(Location(std::move(message_geo_point->geo_)));
      if (m->location.empty()) {
        break;
      }

      return std::move(m);
    }
    case telegram_api::messageMediaGeoLive::ID: {
      auto message_geo_point_live = move_tl_object_as<telegram_api::messageMediaGeoLive>(media);
      int32 period = message_geo_point_live->period_;
      auto location = Location(std::move(message_geo_point_live->geo_));
      if (location.empty()) {
        break;
      }

      if (period <= 0) {
        LOG(ERROR) << "Receive wrong live location period = " << period;
        return make_unique<MessageLocation>(std::move(location));
      }
      return make_unique<MessageLiveLocation>(std::move(location), period);
    }
    case telegram_api::messageMediaVenue::ID: {
      auto message_venue = move_tl_object_as<telegram_api::messageMediaVenue>(media);

      auto m = make_unique<MessageVenue>(Venue(message_venue->geo_, std::move(message_venue->title_),
                                               std::move(message_venue->address_), std::move(message_venue->provider_),
                                               std::move(message_venue->venue_id_)));
      if (m->venue.empty()) {
        break;
      }

      return std::move(m);
    }
    case telegram_api::messageMediaContact::ID: {
      auto message_contact = move_tl_object_as<telegram_api::messageMediaContact>(media);
      if (message_contact->user_id_ != 0) {
        td_->contacts_manager_->get_user_id_object(UserId(message_contact->user_id_),
                                                   "messageMediaContact");  // to ensure updateUser
      }
      return make_unique<MessageContact>(Contact(message_contact->phone_number_, message_contact->first_name_,
                                                 message_contact->last_name_, message_contact->user_id_));
    }
    case telegram_api::messageMediaDocument::ID: {
      auto message_document = move_tl_object_as<telegram_api::messageMediaDocument>(media);
      if ((message_document->flags_ & telegram_api::messageMediaDocument::DOCUMENT_MASK) == 0) {
        if ((message_document->flags_ & telegram_api::messageMediaDocument::TTL_SECONDS_MASK) == 0) {
          LOG(ERROR) << "Receive messageMediaDocument without document and TTL: "
                     << oneline(to_string(message_document));
          break;
        }

        return make_unique<MessageExpiredVideo>();
      }

      auto document_ptr = std::move(message_document->document_);
      int32 document_id = document_ptr->get_id();
      if (document_id == telegram_api::documentEmpty::ID) {
        break;
      }
      CHECK(document_id == telegram_api::document::ID);

      if (ttl != nullptr && (message_document->flags_ & telegram_api::messageMediaDocument::TTL_SECONDS_MASK) != 0) {
        *ttl = message_document->ttl_seconds_;
      }
      return get_message_document(move_tl_object_as<telegram_api::document>(document_ptr), owner_dialog_id,
                                  std::move(message), is_content_read, nullptr);
    }
    case telegram_api::messageMediaGame::ID: {
      auto message_game = move_tl_object_as<telegram_api::messageMediaGame>(media);

      auto m = make_unique<MessageGame>(Game(td_, std::move(message_game->game_), owner_dialog_id));
      if (m->game.empty()) {
        break;
      }

      m->game.set_bot_user_id(via_bot_user_id);
      m->game.set_message_text(std::move(message));

      return std::move(m);
    }
    case telegram_api::messageMediaInvoice::ID: {
      auto message_invoice = move_tl_object_as<telegram_api::messageMediaInvoice>(media);

      MessageId receipt_message_id;
      if ((message_invoice->flags_ & telegram_api::messageMediaInvoice::RECEIPT_MSG_ID_MASK) != 0) {
        receipt_message_id = MessageId(ServerMessageId(message_invoice->receipt_msg_id_));
        if (!receipt_message_id.is_valid()) {
          LOG(ERROR) << "Receive as receipt message " << receipt_message_id << " in " << owner_dialog_id;
          receipt_message_id = MessageId();
        }
      }
      bool need_shipping_address =
          (message_invoice->flags_ & telegram_api::messageMediaInvoice::SHIPPING_ADDRESS_REQUESTED_MASK) != 0;
      bool is_test = (message_invoice->flags_ & telegram_api::messageMediaInvoice::TEST_MASK) != 0;
      return make_unique<MessageInvoice>(std::move(message_invoice->title_), std::move(message_invoice->description_),
                                         get_web_document_photo(std::move(message_invoice->photo_), owner_dialog_id),
                                         std::move(message_invoice->start_param_), message_invoice->total_amount_,
                                         std::move(message_invoice->currency_), is_test, need_shipping_address,
                                         receipt_message_id);
    }
    case telegram_api::messageMediaWebPage::ID: {
      auto media_web_page = move_tl_object_as<telegram_api::messageMediaWebPage>(media);
      auto web_page_id = td_->web_pages_manager_->on_get_web_page(std::move(media_web_page->webpage_), owner_dialog_id);
      return make_unique<MessageText>(std::move(message), web_page_id);
    }

    case telegram_api::messageMediaUnsupported::ID: {
      return make_unique<MessageUnsupported>();
    }
    default:
      UNREACHABLE();
  }

  // explicit empty media message
  return make_unique<MessageText>(std::move(message), WebPageId());
}

unique_ptr<MessageContent> MessagesManager::dup_message_content(DialogId dialog_id, const MessageContent *content,
                                                                bool for_forward) {
  CHECK(content != nullptr);

  bool to_secret = dialog_id.get_type() == DialogType::SecretChat;
  auto fix_file_id = [dialog_id, to_secret, file_manager = td_->file_manager_.get()](FileId file_id) {
    auto file_view = file_manager->get_file_view(file_id);
    if (to_secret && !file_view.is_encrypted()) {
      auto download_file_id = file_manager->dup_file_id(file_id);
      file_id = file_manager
                    ->register_generate(FileType::Encrypted, FileLocationSource::FromServer, "",
                                        PSTRING() << "#file_id#" << download_file_id.get(), dialog_id, file_view.size())
                    .ok();
    }
    return file_manager->dup_file_id(file_id);
  };

  FileId thumbnail_file_id;
  if (to_secret) {
    thumbnail_file_id = get_message_content_thumbnail_file_id(content);
  }
  switch (content->get_id()) {
    case MessageAnimation::ID: {
      auto result = make_unique<MessageAnimation>(*static_cast<const MessageAnimation *>(content));
      if (td_->documents_manager_->has_input_media(result->file_id, thumbnail_file_id, to_secret)) {
        return std::move(result);
      }
      result->file_id = td_->animations_manager_->dup_animation(fix_file_id(result->file_id), result->file_id);
      CHECK(result->file_id.is_valid());
      return std::move(result);
    }
    case MessageAudio::ID: {
      auto result = make_unique<MessageAudio>(*static_cast<const MessageAudio *>(content));
      if (td_->documents_manager_->has_input_media(result->file_id, thumbnail_file_id, to_secret)) {
        return std::move(result);
      }
      result->file_id = td_->audios_manager_->dup_audio(fix_file_id(result->file_id), result->file_id);
      CHECK(result->file_id.is_valid());
      return std::move(result);
    }
    case MessageContact::ID:
      return make_unique<MessageContact>(*static_cast<const MessageContact *>(content));
    case MessageDocument::ID: {
      auto result = make_unique<MessageDocument>(*static_cast<const MessageDocument *>(content));
      if (td_->documents_manager_->has_input_media(result->file_id, thumbnail_file_id, to_secret)) {
        return std::move(result);
      }
      result->file_id = td_->documents_manager_->dup_document(fix_file_id(result->file_id), result->file_id);
      CHECK(result->file_id.is_valid());
      return std::move(result);
    }
    case MessageGame::ID:
      return make_unique<MessageGame>(*static_cast<const MessageGame *>(content));
    case MessageInvoice::ID:
      return make_unique<MessageInvoice>(*static_cast<const MessageInvoice *>(content));
    case MessageLiveLocation::ID:
      if (to_secret || for_forward) {
        return make_unique<MessageLocation>(Location(static_cast<const MessageLiveLocation *>(content)->location));
      } else {
        return make_unique<MessageLiveLocation>(*static_cast<const MessageLiveLocation *>(content));
      }
    case MessageLocation::ID:
      return make_unique<MessageLocation>(*static_cast<const MessageLocation *>(content));
    case MessagePhoto::ID: {
      auto result = make_unique<MessagePhoto>(*static_cast<const MessagePhoto *>(content));

      if (result->photo.photos.size() > 2 && !to_secret) {
        // already sent photo
        return std::move(result);
      }

      // Find 'i' or largest
      CHECK(!result->photo.photos.empty());
      PhotoSize photo;
      for (const auto &size : result->photo.photos) {
        if (size.type == 'i') {
          photo = size;
        }
      }
      if (photo.type == 0) {
        for (const auto &size : result->photo.photos) {
          if (photo.type == 0 || photo.size < size.size) {
            photo = size;
          }
        }
      }

      // Find 't' or smallest
      PhotoSize thumbnail;
      for (const auto &size : result->photo.photos) {
        if (size.type == 't') {
          thumbnail = size;
        }
      }
      if (thumbnail.type == 0) {
        for (const auto &size : result->photo.photos) {
          if (size.type != photo.type && (thumbnail.type == 0 || thumbnail.size > size.size)) {
            thumbnail = size;
          }
        }
      }

      result->photo.photos.clear();
      if (thumbnail.type != 0) {
        thumbnail.type = 't';
        result->photo.photos.push_back(std::move(thumbnail));
      }
      photo.type = 'i';
      result->photo.photos.push_back(std::move(photo));

      if (photo_has_input_media(td_->file_manager_.get(), result->photo, to_secret)) {
        return std::move(result);
      }

      result->photo.photos.back().file_id = fix_file_id(result->photo.photos.back().file_id);
      if (thumbnail.type != 0) {
        result->photo.photos[0].file_id = td_->file_manager_->dup_file_id(result->photo.photos[0].file_id);
      }
      return std::move(result);
    }
    case MessageSticker::ID: {
      auto result = make_unique<MessageSticker>(*static_cast<const MessageSticker *>(content));
      if (td_->stickers_manager_->has_input_media(result->file_id, to_secret)) {
        return std::move(result);
      }
      result->file_id = td_->stickers_manager_->dup_sticker(fix_file_id(result->file_id), result->file_id);
      CHECK(result->file_id.is_valid());
      return std::move(result);
    }
    case MessageText::ID:
      return make_unique<MessageText>(*static_cast<const MessageText *>(content));
    case MessageVenue::ID:
      return make_unique<MessageVenue>(*static_cast<const MessageVenue *>(content));
    case MessageVideo::ID: {
      auto result = make_unique<MessageVideo>(*static_cast<const MessageVideo *>(content));
      if (td_->documents_manager_->has_input_media(result->file_id, thumbnail_file_id, to_secret)) {
        return std::move(result);
      }
      result->file_id = td_->videos_manager_->dup_video(fix_file_id(result->file_id), result->file_id);
      CHECK(result->file_id.is_valid());
      return std::move(result);
    }
    case MessageVideoNote::ID: {
      auto result = make_unique<MessageVideoNote>(*static_cast<const MessageVideoNote *>(content));
      result->is_viewed = false;
      if (td_->documents_manager_->has_input_media(result->file_id, thumbnail_file_id, to_secret)) {
        return std::move(result);
      }
      result->file_id = td_->video_notes_manager_->dup_video_note(fix_file_id(result->file_id), result->file_id);
      CHECK(result->file_id.is_valid());
      return std::move(result);
    }
    case MessageVoiceNote::ID: {
      auto result = make_unique<MessageVoiceNote>(*static_cast<const MessageVoiceNote *>(content));
      result->is_listened = false;
      if (td_->documents_manager_->has_input_media(result->file_id, thumbnail_file_id, to_secret)) {
        return std::move(result);
      }
      result->file_id = td_->voice_notes_manager_->dup_voice_note(fix_file_id(result->file_id), result->file_id);
      CHECK(result->file_id.is_valid());
      return std::move(result);
    }
    case MessageUnsupported::ID:
    case MessageChatCreate::ID:
    case MessageChatChangeTitle::ID:
    case MessageChatChangePhoto::ID:
    case MessageChatDeletePhoto::ID:
    case MessageChatDeleteHistory::ID:
    case MessageChatAddUsers::ID:
    case MessageChatJoinedByLink::ID:
    case MessageChatDeleteUser::ID:
    case MessageChatMigrateTo::ID:
    case MessageChannelCreate::ID:
    case MessageChannelMigrateFrom::ID:
    case MessagePinMessage::ID:
    case MessageGameScore::ID:
    case MessageScreenshotTaken::ID:
    case MessageChatSetTtl::ID:
    case MessageCall::ID:
    case MessagePaymentSuccessful::ID:
    case MessageContactRegistered::ID:
    case MessageExpiredPhoto::ID:
    case MessageExpiredVideo::ID:
    case MessageCustomServiceAction::ID:
    case MessageWebsiteConnected::ID:
      return nullptr;
    default:
      UNREACHABLE();
  }
  UNREACHABLE();
  return nullptr;
}

unique_ptr<MessageContent> MessagesManager::get_message_action_content(
    tl_object_ptr<telegram_api::MessageAction> &&action, DialogId owner_dialog_id,
    MessageId reply_to_message_id) const {
  CHECK(action != nullptr);

  switch (action->get_id()) {
    case telegram_api::messageActionEmpty::ID:
      LOG(ERROR) << "Receive empty message action";
      break;
    case telegram_api::messageActionChatCreate::ID: {
      auto chat_create = move_tl_object_as<telegram_api::messageActionChatCreate>(action);

      vector<UserId> participant_user_ids;
      participant_user_ids.reserve(chat_create->users_.size());
      for (auto &user : chat_create->users_) {
        UserId user_id(user);
        if (user_id.is_valid()) {
          participant_user_ids.push_back(user_id);
        } else {
          LOG(ERROR) << "Receive invalid " << user_id;
        }
      }

      return make_unique<MessageChatCreate>(std::move(chat_create->title_), std::move(participant_user_ids));
    }
    case telegram_api::messageActionChatEditTitle::ID: {
      auto chat_edit_title = move_tl_object_as<telegram_api::messageActionChatEditTitle>(action);
      return make_unique<MessageChatChangeTitle>(std::move(chat_edit_title->title_));
    }
    case telegram_api::messageActionChatEditPhoto::ID: {
      auto chat_edit_photo = move_tl_object_as<telegram_api::messageActionChatEditPhoto>(action);

      auto photo_ptr = std::move(chat_edit_photo->photo_);
      int32 photo_id = photo_ptr->get_id();
      if (photo_id == telegram_api::photoEmpty::ID) {
        break;
      }
      CHECK(photo_id == telegram_api::photo::ID);

      return make_unique<MessageChatChangePhoto>(
          get_photo(td_->file_manager_.get(), move_tl_object_as<telegram_api::photo>(photo_ptr), owner_dialog_id));
    }
    case telegram_api::messageActionChatDeletePhoto::ID: {
      return make_unique<MessageChatDeletePhoto>();
    }
    case telegram_api::messageActionHistoryClear::ID: {
      return make_unique<MessageChatDeleteHistory>();
    }
    case telegram_api::messageActionChatAddUser::ID: {
      auto chat_add_user = move_tl_object_as<telegram_api::messageActionChatAddUser>(action);

      vector<UserId> user_ids;
      user_ids.reserve(chat_add_user->users_.size());
      for (auto &user : chat_add_user->users_) {
        UserId user_id(user);
        if (user_id.is_valid()) {
          user_ids.push_back(user_id);
        } else {
          LOG(ERROR) << "Receive invalid " << user_id;
        }
      }

      return make_unique<MessageChatAddUsers>(std::move(user_ids));
    }
    case telegram_api::messageActionChatJoinedByLink::ID:
      return make_unique<MessageChatJoinedByLink>();
    case telegram_api::messageActionChatDeleteUser::ID: {
      auto chat_delete_user = move_tl_object_as<telegram_api::messageActionChatDeleteUser>(action);

      UserId user_id(chat_delete_user->user_id_);
      if (!user_id.is_valid()) {
        LOG(ERROR) << "Receive invalid " << user_id;
        break;
      }

      return make_unique<MessageChatDeleteUser>(user_id);
    }
    case telegram_api::messageActionChatMigrateTo::ID: {
      auto chat_migrate_to = move_tl_object_as<telegram_api::messageActionChatMigrateTo>(action);

      ChannelId migrated_to_channel_id(chat_migrate_to->channel_id_);
      if (!migrated_to_channel_id.is_valid()) {
        LOG(ERROR) << "Receive invalid " << migrated_to_channel_id;
        break;
      }

      return make_unique<MessageChatMigrateTo>(migrated_to_channel_id);
    }
    case telegram_api::messageActionChannelCreate::ID: {
      auto channel_create = move_tl_object_as<telegram_api::messageActionChannelCreate>(action);
      return make_unique<MessageChannelCreate>(std::move(channel_create->title_));
    }
    case telegram_api::messageActionChannelMigrateFrom::ID: {
      auto channel_migrate_from = move_tl_object_as<telegram_api::messageActionChannelMigrateFrom>(action);

      ChatId chat_id(channel_migrate_from->chat_id_);
      LOG_IF(ERROR, !chat_id.is_valid()) << "Receive invalid " << chat_id;

      return make_unique<MessageChannelMigrateFrom>(std::move(channel_migrate_from->title_), chat_id);
    }
    case telegram_api::messageActionPinMessage::ID: {
      if (!reply_to_message_id.is_valid()) {
        LOG(ERROR) << "Receive pinned message with " << reply_to_message_id;
        reply_to_message_id = MessageId();
      }
      return make_unique<MessagePinMessage>(reply_to_message_id);
    }
    case telegram_api::messageActionGameScore::ID: {
      if (!reply_to_message_id.is_valid()) {
        LOG_IF(ERROR, !td_->auth_manager_->is_bot()) << "Receive game score with " << reply_to_message_id;
        reply_to_message_id = MessageId();
      }
      auto game_score = move_tl_object_as<telegram_api::messageActionGameScore>(action);
      return make_unique<MessageGameScore>(reply_to_message_id, game_score->game_id_, game_score->score_);
    }
    case telegram_api::messageActionPhoneCall::ID: {
      auto phone_call = move_tl_object_as<telegram_api::messageActionPhoneCall>(action);
      auto duration =
          (phone_call->flags_ & telegram_api::messageActionPhoneCall::DURATION_MASK) != 0 ? phone_call->duration_ : 0;
      return make_unique<MessageCall>(phone_call->call_id_, duration, get_call_discard_reason(phone_call->reason_));
    }
    case telegram_api::messageActionPaymentSent::ID: {
      LOG_IF(ERROR, td_->auth_manager_->is_bot()) << "Receive MessageActionPaymentSent";
      if (!reply_to_message_id.is_valid()) {
        LOG(ERROR) << "Receive succesful payment message with " << reply_to_message_id;
        reply_to_message_id = MessageId();
      }
      auto payment_sent = move_tl_object_as<telegram_api::messageActionPaymentSent>(action);
      return make_unique<MessagePaymentSuccessful>(reply_to_message_id, std::move(payment_sent->currency_),
                                                   payment_sent->total_amount_);
    }
    case telegram_api::messageActionPaymentSentMe::ID: {
      LOG_IF(ERROR, !td_->auth_manager_->is_bot()) << "Receive MessageActionPaymentSentMe";
      if (!reply_to_message_id.is_valid()) {
        LOG(ERROR) << "Receive succesful payment message with " << reply_to_message_id;
        reply_to_message_id = MessageId();
      }
      auto payment_sent = move_tl_object_as<telegram_api::messageActionPaymentSentMe>(action);
      auto result = make_unique<MessagePaymentSuccessful>(reply_to_message_id, std::move(payment_sent->currency_),
                                                          payment_sent->total_amount_);
      result->invoice_payload = payment_sent->payload_.as_slice().str();
      result->shipping_option_id = std::move(payment_sent->shipping_option_id_);
      result->order_info = get_order_info(std::move(payment_sent->info_));
      result->telegram_payment_charge_id = std::move(payment_sent->charge_->id_);
      result->provider_payment_charge_id = std::move(payment_sent->charge_->provider_charge_id_);
      return std::move(result);
    }
    case telegram_api::messageActionScreenshotTaken::ID: {
      return make_unique<MessageScreenshotTaken>();
    }
    case telegram_api::messageActionCustomAction::ID: {
      auto custom_action = move_tl_object_as<telegram_api::messageActionCustomAction>(action);
      return make_unique<MessageCustomServiceAction>(std::move(custom_action->message_));
    }
    case telegram_api::messageActionBotAllowed::ID: {
      auto bot_allowed = move_tl_object_as<telegram_api::messageActionBotAllowed>(action);
      return make_unique<MessageWebsiteConnected>(std::move(bot_allowed->domain_));
    }
    default:
      UNREACHABLE();
  }
  // explicit empty or wrong action
  return make_unique<MessageText>(FormattedText(), WebPageId());
}

int32 MessagesManager::get_random_y(MessageId message_id) {
  return static_cast<int32>(static_cast<uint32>(message_id.get() * 2101234567u));
}

MessagesManager::Message *MessagesManager::add_message_to_dialog(DialogId dialog_id, unique_ptr<Message> message,
                                                                 bool from_update, bool *need_update,
                                                                 bool *need_update_dialog_pos, const char *source) {
  CHECK(message != nullptr);
  CHECK(dialog_id.get_type() != DialogType::None);
  CHECK(need_update_dialog_pos != nullptr);

  MessageId message_id = message->message_id;
  if (!message_id.is_valid()) {
    LOG(ERROR) << "Receive " << message_id << " in " << dialog_id << " from " << source;
    debug_add_message_to_dialog_fail_reason = "invalid message id";
    return nullptr;
  }

  // TODO remove creation of dialog from this function, use cgc or cpc or something else
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    d = add_dialog(dialog_id);
    *need_update_dialog_pos = true;
  } else {
    CHECK(d->dialog_id == dialog_id);
  }
  return add_message_to_dialog(d, std::move(message), from_update, need_update, need_update_dialog_pos, source);
}

MessagesManager::Message *MessagesManager::add_message_to_dialog(Dialog *d, unique_ptr<Message> message,
                                                                 bool from_update, bool *need_update,
                                                                 bool *need_update_dialog_pos, const char *source) {
  CHECK(message != nullptr);
  CHECK(d != nullptr);
  CHECK(need_update != nullptr);
  CHECK(need_update_dialog_pos != nullptr);
  CHECK(source != nullptr);

  DialogId dialog_id = d->dialog_id;
  MessageId message_id = message->message_id;

  if (d->deleted_message_ids.count(message_id)) {
    LOG(INFO) << "Skip adding deleted " << message_id << " to " << dialog_id << " from " << source;
    debug_add_message_to_dialog_fail_reason = "adding deleted message";
    return nullptr;
  }
  if (message_id.get() <= d->last_clear_history_message_id.get()) {
    LOG(INFO) << "Skip adding cleared " << message_id << " to " << dialog_id << " from " << source;
    debug_add_message_to_dialog_fail_reason = "cleared full history";
    return nullptr;
  }
  if (d->deleted_message_ids.count(message->reply_to_message_id)) {
    // LOG(INFO) << "Remove reply to deleted " << message->reply_to_message_id << " in " << message_id << " from " << dialog_id << " from " << source;
    // we don't want to lose information that the message was a reply for now
    // message->reply_to_message_id = MessageId();
  }

  LOG(INFO) << "Adding " << message_id << " of type " << message->content->get_id() << " to " << dialog_id << " from "
            << source << ". Last new is " << d->last_new_message_id << ", last is " << d->last_message_id
            << ", from_update = " << from_update << ", have_previous = " << message->have_previous
            << ", have_next = " << message->have_next;

  if (!message_id.is_valid()) {
    LOG(ERROR) << "Receive " << message_id << " in " << dialog_id << " from " << source;
    CHECK(!message->from_database);
    debug_add_message_to_dialog_fail_reason = "invalid message id";
    return nullptr;
  }

  auto message_content_id = message->content->get_id();
  if (is_debug_message_op_enabled()) {
    d->debug_message_op.emplace_back(Dialog::MessageOp::Add, message_id, message_content_id, from_update,
                                     message->have_previous, message->have_next, source);
  }

  message->last_access_date = G()->unix_time_cached();

  if (*need_update) {
    CHECK(from_update);
  }
  if (from_update) {
    CHECK(message->have_next);
    CHECK(message->have_previous);
    CHECK(!message_id.is_yet_unsent());
    if (message_id.get() <= d->last_new_message_id.get() && d->dialog_id.get_type() != DialogType::Channel) {
      if (!G()->parameters().use_message_db) {
        LOG(ERROR) << "New " << message_id << " in " << dialog_id << " from " << source
                   << " has id less than last_new_message_id = " << d->last_new_message_id;
        dump_debug_message_op(d);
      }
    }
  }

  if (!from_update && message_id.is_server() && d->last_new_message_id != MessageId() &&
      message_id.get() > d->last_new_message_id.get()) {
    if (!message->from_database) {
      LOG(ERROR) << "Ignore " << message_id << " in " << dialog_id << " received not through update from " << source
                 << ". Last new is " << d->last_new_message_id << ", "
                 << to_string(get_message_object(dialog_id, message.get()));
      dump_debug_message_op(d, 3);
      if (dialog_id.get_type() == DialogType::Channel && have_input_peer(dialog_id, AccessRights::Read)) {
        channel_get_difference_retry_timeout_.add_timeout_in(dialog_id.get(), 0.001);
      }
    } else {
      LOG(INFO) << "Ignore " << message_id << " in " << dialog_id << " received not through update from " << source;
    }
    debug_add_message_to_dialog_fail_reason = "too new message not from update";
    return nullptr;
  }
  if ((message_id.is_server() || (message_id.is_local() && dialog_id.get_type() == DialogType::SecretChat)) &&
      message_id.get() <= d->max_unavailable_message_id.get()) {
    LOG(INFO) << "Can't add an unavailable " << message_id << " to " << dialog_id << " from " << source;
    if (message->from_database) {
      delete_message_from_database(d, message_id, message.get(), true);
    }
    debug_add_message_to_dialog_fail_reason = "ignore unavailable message";
    return nullptr;
  }

  if (message_content_id == MessageText::ID) {
    auto web_page_id = static_cast<const MessageText *>(message->content.get())->web_page_id;
    if (web_page_id.is_valid() && !td_->web_pages_manager_->have_web_page(web_page_id)) {
      waiting_for_web_page_messages_.emplace(dialog_id, message_id);
      send_closure(G()->web_pages_manager(), &WebPagesManager::wait_for_pending_web_page, dialog_id, message_id,
                   web_page_id);
    }
  }

  if (*need_update && message_id.get() <= d->last_new_message_id.get()) {
    *need_update = false;
  }

  bool auto_attach = message->have_previous && message->have_next &&
                     (from_update || message_id.is_local() || message_id.is_yet_unsent());
  if (message_content_id == MessageChatDeleteHistory::ID) {
    auto m = delete_message(d, message_id, true, need_update_dialog_pos, "message chat delete history");
    if (m != nullptr) {
      send_update_delete_messages(dialog_id, {m->message_id.get()}, true, false);
    }
    int32 last_message_date = 0;
    if (d->last_message_id != MessageId()) {
      auto last_message = get_message(d, d->last_message_id);
      CHECK(last_message != nullptr);
      last_message_date = last_message->date - 1;
    } else {
      last_message_date = d->last_clear_history_date;
    }
    if (message->date > last_message_date) {
      set_dialog_last_clear_history_date(d, message->date, message_id, "update_last_clear_history_date");
      on_dialog_updated(dialog_id, "update_last_clear_history_date");
      *need_update_dialog_pos = true;
    }
    LOG(INFO) << "Process MessageChatDeleteHistory in " << message_id << " in " << dialog_id << " with date "
              << message->date << " from " << source;
    CHECK(!message->from_database);
    debug_add_message_to_dialog_fail_reason = "skip adding MessageChatDeleteHistory";
    return nullptr;
  }

  if (!message->from_database) {
    // load message from database before updating it
    get_message_force(d, message_id);
  }

  if (message->reply_markup != nullptr &&
      (message->reply_markup->type == ReplyMarkup::Type::RemoveKeyboard ||
       (message->reply_markup->type == ReplyMarkup::Type::ForceReply && !message->reply_markup->is_personal)) &&
      !td_->auth_manager_->is_bot()) {
    if (*need_update && message->reply_markup->is_personal) {  // if this keyboard is for us
      if (d->reply_markup_message_id != MessageId()) {
        const Message *old_message = get_message_force(d, d->reply_markup_message_id);
        if (old_message == nullptr || old_message->sender_user_id == message->sender_user_id) {
          set_dialog_reply_markup(d, MessageId());
        }
      }
    }
    message->had_reply_markup = message->reply_markup->is_personal;
    message->reply_markup = nullptr;
  }

  if (from_update) {
    cancel_user_dialog_action(dialog_id, message.get());
  }

  unique_ptr<Message> *v = &d->messages;
  while (*v != nullptr && (*v)->random_y >= message->random_y) {
    if ((*v)->message_id.get() < message_id.get()) {
      v = &(*v)->right;
    } else if ((*v)->message_id == message_id) {
      LOG(INFO) << "Adding already existing " << message_id << " in " << dialog_id << " from " << source;
      if (*need_update) {
        *need_update = false;
        if (!G()->parameters().use_message_db) {
          LOG(ERROR) << "Receive again " << (message->is_outgoing ? "outgoing" : "incoming")
                     << (message->forward_info == nullptr ? " not" : "") << " forwarded " << message_id
                     << " with content of type " << message_content_id << " in " << dialog_id << " from " << source
                     << ", current last new is " << d->last_new_message_id << ", last is " << d->last_message_id << ". "
                     << td_->updates_manager_->get_state();
          dump_debug_message_op(d, 1);
        }
      }
      if (auto_attach) {
        CHECK(message->have_previous);
        CHECK(message->have_next);
        message->have_previous = false;
        message->have_next = false;
      }
      if (!message->from_database) {
        bool was_deleted = delete_active_live_location(dialog_id, v->get());
        update_message(d, *v, std::move(message), true, need_update_dialog_pos);
        if (was_deleted) {
          try_add_active_live_location(dialog_id, v->get());
        }
      }
      return v->get();
    } else {
      v = &(*v)->left;
    }
  }

  if (d->have_full_history && !message->from_database && !from_update && !message_id.is_local() &&
      !message_id.is_yet_unsent()) {
    LOG(ERROR) << "Have full history in " << dialog_id << ", but receive unknown " << message_id << " from " << source
               << ". Last new is " << d->last_new_message_id << ", last is " << d->last_message_id
               << ", first database is " << d->first_database_message_id << ", last database is "
               << d->last_database_message_id << ", last read inbox is " << d->last_read_inbox_message_id
               << ", last read outbox is " << d->last_read_inbox_message_id << ", last read all mentions is "
               << d->last_read_all_mentions_message_id << ", max unavailable is " << d->max_unavailable_message_id
               << ", last assigned is " << d->last_assigned_message_id;
    d->have_full_history = false;
    on_dialog_updated(dialog_id, "drop have_full_history");
  }

  if (!d->is_opened && d->messages != nullptr && is_message_unload_enabled()) {
    LOG(INFO) << "Schedule unload of " << dialog_id;
    pending_unload_dialog_timeout_.add_timeout_in(dialog_id.get(), DIALOG_UNLOAD_DELAY);
  }

  if (message->ttl > 0 && message->ttl_expires_at != 0) {
    auto now = Time::now();
    if (message->ttl_expires_at <= now) {
      if (d->dialog_id.get_type() == DialogType::SecretChat) {
        LOG(INFO) << "Can't add " << message_id << " with expired TTL to " << dialog_id << " from " << source;
        delete_message_from_database(d, message_id, message.get(), true);
        debug_add_message_to_dialog_fail_reason = "delete expired by TTL message";
        return nullptr;
      } else {
        on_message_ttl_expired_impl(d, message.get());
        message_content_id = message->content->get_id();
        if (message->from_database) {
          add_message_to_database(d, message.get(), "add expired message to dialog");
        }
      }
    } else {
      ttl_register_message(dialog_id, message.get(), now);
    }
  }

  LOG(INFO) << "Adding not found " << message_id << " to " << dialog_id << " from " << source;
  if (d->is_empty) {
    d->is_empty = false;
    *need_update_dialog_pos = true;
  }

  if (dialog_id.get_type() == DialogType::Channel && !message->contains_unread_mention &&
      message->date <
          G()->unix_time_cached() - G()->shared_config().get_option_integer("channels_read_media_period",
                                                                            (G()->is_test_dc() ? 300 : 7 * 86400))) {
    update_opened_message_content(message.get());
  }

  if (message->contains_unread_mention && message_id.get() <= d->last_read_all_mentions_message_id.get()) {
    message->contains_unread_mention = false;
  }

  if (message_id.is_yet_unsent() && message->reply_to_message_id.is_valid() &&
      !message->reply_to_message_id.is_yet_unsent()) {
    replied_by_yet_unsent_messages_[FullMessageId{dialog_id, message->reply_to_message_id}]++;
  }

  if (G()->parameters().use_file_db && message_id.is_yet_unsent() && !message->via_bot_user_id.is_valid()) {
    auto queue_id = get_sequence_dispatcher_id(dialog_id, message_content_id);
    if (queue_id & 1) {
      LOG(INFO) << "Add " << message_id << " from " << source << " to queue " << queue_id;
      yet_unsent_media_queues_[queue_id][message_id.get()];  // reserve place for promise
      if (!td_->auth_manager_->is_bot() && !is_broadcast_channel(dialog_id)) {
        pending_send_dialog_action_timeout_.add_timeout_in(dialog_id.get(), 1.0);
      }
    }
  }

  if (from_update && message_id.get() > d->last_new_message_id.get()) {
    CHECK(!message_id.is_yet_unsent());
    if (d->dialog_id.get_type() == DialogType::SecretChat || message_id.is_server()) {
      // can delete messages, therefore must be called before message attaching/adding
      set_dialog_last_new_message_id(d, message_id, "add_message_to_dialog");
    }
  }
  if (!(d->have_full_history && auto_attach) && d->last_message_id.is_valid() &&
      d->last_message_id.get() < MessageId(ServerMessageId(1)).get() &&
      message_id.get() >= MessageId(ServerMessageId(1)).get()) {
    set_dialog_last_message_id(d, MessageId(), "add_message_to_dialog");

    set_dialog_first_database_message_id(d, MessageId(), "add_message_to_dialog");
    set_dialog_last_database_message_id(d, MessageId(), "add_message_to_dialog");
    d->have_full_history = false;
    for (auto &first_message_id : d->first_database_message_id_by_index) {
      first_message_id = MessageId();
    }
    std::fill(d->message_count_by_index.begin(), d->message_count_by_index.end(), -1);
    d->local_unread_count = 0;  // read all local messages. They will not be reachable anymore

    on_dialog_updated(dialog_id, "add gap to dialog");

    send_update_chat_last_message(d, "add gap to dialog");
    *need_update_dialog_pos = false;
  }

  bool is_attached = false;
  if (auto_attach) {
    LOG(INFO) << "Trying to auto attach " << message_id;
    auto it = MessagesIterator(d, message_id);
    Message *previous_message = *it;
    if (previous_message != nullptr) {
      auto previous_message_id = previous_message->message_id;
      CHECK(previous_message_id.get() < message_id.get());
      if (previous_message->have_next ||
          (d->last_message_id.is_valid() && previous_message_id.get() >= d->last_message_id.get())) {
        if (message_id.is_server() && previous_message_id.is_server() && previous_message->have_next) {
          ++it;
          auto next_message = *it;
          if (next_message != nullptr) {
            if (next_message->message_id.is_server()) {
              LOG(ERROR) << "Attach " << message_id << " from " << source << " before " << next_message->message_id
                         << " and after " << previous_message_id << " in " << dialog_id;
              dump_debug_message_op(d);
            }
          } else {
            LOG(ERROR) << "Have_next is true, but there is no next message after " << previous_message_id << " from "
                       << source << " in " << dialog_id;
            dump_debug_message_op(d);
          }
        }

        LOG(INFO) << "Attach " << message_id << " to the previous " << previous_message_id;
        message->have_previous = true;
        message->have_next = previous_message->have_next;
        previous_message->have_next = true;
        is_attached = true;
      }
    }
    if (!is_attached && !message_id.is_yet_unsent()) {
      // message may be attached to the next message if there is no previous message
      Message *cur = d->messages.get();
      Message *next_message = nullptr;
      while (cur != nullptr) {
        if (cur->message_id.get() < message_id.get()) {
          cur = cur->right.get();
        } else {
          next_message = cur;
          cur = cur->left.get();
        }
      }
      if (next_message != nullptr) {
        CHECK(!next_message->have_previous);
        LOG(INFO) << "Attach " << message_id << " to the next " << next_message->message_id;
        LOG_IF(ERROR, from_update) << "Attach " << message_id << " from " << source << " to the next "
                                   << next_message->message_id << " in " << dialog_id;
        message->have_next = true;
        message->have_previous = next_message->have_previous;
        next_message->have_previous = true;
        is_attached = true;
      }
    }
    if (!is_attached) {
      LOG(INFO) << "Can't attach " << message_id;
      message->have_previous = false;
      message->have_next = false;
    }
  }

  UserId my_user_id(td_->contacts_manager_->get_my_id("add_message_to_dialog"));
  DialogId my_dialog_id(my_user_id);
  if (*need_update && message_id.get() > d->last_read_inbox_message_id.get() && !td_->auth_manager_->is_bot()) {
    if (!message->is_outgoing && dialog_id != my_dialog_id) {
      int32 server_unread_count = d->server_unread_count;
      int32 local_unread_count = d->local_unread_count;
      if (message_id.is_server()) {
        server_unread_count++;
      } else {
        local_unread_count++;
      }
      set_dialog_last_read_inbox_message_id(d, MessageId::min(), server_unread_count, local_unread_count, false,
                                            source);
    } else {
      // if outgoing message has id one greater than last_read_inbox_message_id than definitely there is no
      // unread incoming message before it
      if (message_id.is_server() && d->last_read_inbox_message_id.is_valid() &&
          d->last_read_inbox_message_id.is_server() &&
          message_id.get_server_message_id().get() == d->last_read_inbox_message_id.get_server_message_id().get() + 1) {
        read_history_inbox(d->dialog_id, message_id, 0, "add_message_to_dialog");
      }
    }
  }
  if (*need_update && message->contains_unread_mention) {
    d->unread_mention_count++;
    d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] =
        d->unread_mention_count;
    send_update_chat_unread_mention_count(d);
  }
  if (*need_update) {
    update_message_count_by_index(d, +1, message.get());
  }
  if (auto_attach && message_id.get() > d->last_message_id.get() && message_id.get() >= d->last_new_message_id.get()) {
    set_dialog_last_message_id(d, message_id, "add_message_to_dialog");
    *need_update_dialog_pos = true;
  }
  if (auto_attach && !message_id.is_yet_unsent() && message_id.get() >= d->last_new_message_id.get() &&
      (d->last_new_message_id.is_valid() || (message_id.is_local() && message_id.get() >= d->last_message_id.get()))) {
    CHECK(message_id.get() <= d->last_message_id.get());
    if (message_id.get() > d->last_database_message_id.get()) {
      set_dialog_last_database_message_id(d, message_id, "add_message_to_dialog");
      if (!d->first_database_message_id.is_valid()) {
        set_dialog_first_database_message_id(d, message_id, "add_message_to_dialog");
        try_restore_dialog_reply_markup(d, message.get());
      }
      on_dialog_updated(dialog_id, "update_last_database_message_id");
    }
  }
  if (!message->from_database && !message_id.is_yet_unsent()) {
    add_message_to_database(d, message.get(), "add_message_to_dialog");
  }

  if (!is_attached && !message->have_next && !message->have_previous) {
    MessagesIterator it(d, message_id);
    if (*it != nullptr && (*it)->have_next) {
      // need to drop a connection between messages
      auto previous_message = *it;
      ++it;
      auto next_message = *it;
      if (next_message != nullptr) {
        if (next_message->message_id.is_server() &&
            !(td_->auth_manager_->is_bot() && Slice(source) == Slice("get channel messages"))) {
          LOG(ERROR) << "Can't attach " << message_id << " from " << source << " before " << next_message->message_id
                     << " and after " << previous_message->message_id << " in " << dialog_id;
          dump_debug_message_op(d);
        }

        next_message->have_previous = false;
        previous_message->have_next = false;
      } else {
        LOG(ERROR) << "Have_next is true, but there is no next message after " << previous_message->message_id
                   << " from " << source << " in " << dialog_id;
        dump_debug_message_op(d);
      }
    }
  }

  if (!td_->auth_manager_->is_bot() && from_update && d->reply_markup_message_id != MessageId() &&
      message_content_id == MessageChatDeleteUser::ID) {
    auto deleted_user_id = static_cast<const MessageChatDeleteUser *>(message->content.get())->user_id;
    if (td_->contacts_manager_->is_user_bot(deleted_user_id)) {
      const Message *old_message = get_message_force(d, d->reply_markup_message_id);
      if (old_message == nullptr || old_message->sender_user_id == deleted_user_id) {
        set_dialog_reply_markup(d, MessageId());
      }
    }
  }

  if (message_content_id == MessageContactRegistered::ID && !d->has_contact_registered_message) {
    d->has_contact_registered_message = true;
    on_dialog_updated(dialog_id, "update_has_contact_registered_message");
  }

  if (from_update && dialog_id.get_type() == DialogType::Channel) {
    int32 new_participant_count = 0;
    switch (message_content_id) {
      case MessageChatAddUsers::ID:
        new_participant_count =
            narrow_cast<int32>(static_cast<const MessageChatAddUsers *>(message->content.get())->user_ids.size());
        break;
      case MessageChatJoinedByLink::ID:
        new_participant_count = 1;
        break;
      case MessageChatDeleteUser::ID:
        new_participant_count = -1;
        break;
      case MessagePinMessage::ID:
        td_->contacts_manager_->on_update_channel_pinned_message(
            dialog_id.get_channel_id(), static_cast<const MessagePinMessage *>(message->content.get())->message_id);
        break;
    }
    if (new_participant_count != 0) {
      td_->contacts_manager_->speculative_add_channel_participants(dialog_id.get_channel_id(), new_participant_count,
                                                                   message->sender_user_id == my_user_id);
    }
  }
  if (!td_->auth_manager_->is_bot() && (from_update || message_id.is_yet_unsent()) &&
      message->forward_info == nullptr && (message->is_outgoing || dialog_id == my_dialog_id)) {
    switch (message_content_id) {
      case MessageAnimation::ID:
        if (dialog_id.get_type() != DialogType::SecretChat) {
          td_->animations_manager_->add_saved_animation_by_id(get_message_content_file_id(message->content.get()));
        }
        break;
      case MessageSticker::ID:
        if (dialog_id.get_type() != DialogType::SecretChat) {
          td_->stickers_manager_->add_recent_sticker_by_id(false, get_message_content_file_id(message->content.get()));
        }
        break;
      case MessageText::ID:
        update_used_hashtags(dialog_id, message.get());
        break;
    }
  }
  if (!td_->auth_manager_->is_bot() && from_update && (message->is_outgoing || dialog_id == my_dialog_id) &&
      dialog_id.get_type() != DialogType::SecretChat) {
    if (message->via_bot_user_id.is_valid() && message->forward_info == nullptr) {
      // forwarded game messages can't be distinguished from sent via bot game messages, so increase rating anyway
      send_closure(G()->top_dialog_manager(), &TopDialogManager::on_dialog_used, TopDialogCategory::BotInline,
                   DialogId(message->via_bot_user_id), message->date);
    }

    TopDialogCategory category = TopDialogCategory::Size;
    switch (dialog_id.get_type()) {
      case DialogType::User: {
        if (td_->contacts_manager_->is_user_bot(dialog_id.get_user_id())) {
          category = TopDialogCategory::BotPM;
        } else {
          category = TopDialogCategory::Correspondent;
        }
        break;
      }
      case DialogType::Chat:
        category = TopDialogCategory::Group;
        break;
      case DialogType::Channel:
        switch (td_->contacts_manager_->get_channel_type(dialog_id.get_channel_id())) {
          case ChannelType::Broadcast:
            category = TopDialogCategory::Channel;
            break;
          case ChannelType::Megagroup:
            category = TopDialogCategory::Group;
            break;
          case ChannelType::Unknown:
            break;
          default:
            UNREACHABLE();
            break;
        }
        break;
      case DialogType::SecretChat:
      case DialogType::None:
      default:
        UNREACHABLE();
    }
    if (category != TopDialogCategory::Size) {
      send_closure(G()->top_dialog_manager(), &TopDialogManager::on_dialog_used, category, dialog_id, message->date);
    }
  }

  // TODO function
  v = &d->messages;
  while (*v != nullptr && (*v)->random_y >= message->random_y) {
    if ((*v)->message_id.get() < message_id.get()) {
      v = &(*v)->right;
    } else if ((*v)->message_id == message_id) {
      UNREACHABLE();
    } else {
      v = &(*v)->left;
    }
  }

  unique_ptr<Message> *left = &message->left;
  unique_ptr<Message> *right = &message->right;

  unique_ptr<Message> cur = std::move(*v);
  while (cur != nullptr) {
    if (cur->message_id.get() < message_id.get()) {
      *left = std::move(cur);
      left = &((*left)->right);
      cur = std::move(*left);
    } else {
      *right = std::move(cur);
      right = &((*right)->left);
      cur = std::move(*right);
    }
  }
  CHECK(*left == nullptr);
  CHECK(*right == nullptr);
  *v = std::move(message);

  CHECK(d->messages != nullptr);

  if (!is_attached) {
    if ((*v)->have_next) {
      CHECK(!(*v)->have_previous);
      attach_message_to_next(d, message_id);
    } else if ((*v)->have_previous) {
      attach_message_to_previous(d, message_id);
    }
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      message_id_to_dialog_id_[message_id] = dialog_id;
      break;
    case DialogType::Channel:
      // nothing to do
      break;
    case DialogType::SecretChat:
      LOG(INFO) << "Add correspondence random_id " << (*v)->random_id << " to " << message_id << " in " << dialog_id;
      d->random_id_to_message_id[(*v)->random_id] = message_id;
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  return v->get();
}

void MessagesManager::on_message_changed(const Dialog *d, const Message *m, const char *source) {
  CHECK(d != nullptr);
  CHECK(m != nullptr);
  if (m->message_id == d->last_message_id) {
    send_update_chat_last_message_impl(d, source);
  }

  if (m->message_id == d->last_database_message_id) {
    on_dialog_updated(d->dialog_id, source);
  }

  if (!m->message_id.is_yet_unsent()) {
    add_message_to_database(d, m, source);
  }
}

void MessagesManager::add_message_to_database(const Dialog *d, const Message *m, const char *source) {
  if (!G()->parameters().use_message_db) {
    return;
  }

  CHECK(d != nullptr);
  CHECK(m != nullptr);
  MessageId message_id = m->message_id;
  CHECK(message_id.is_server() || message_id.is_local()) << source;

  LOG(INFO) << "Add " << FullMessageId(d->dialog_id, message_id) << " to database from " << source;

  ServerMessageId unique_message_id;
  int64 random_id = 0;
  int64 search_id = 0;
  string text;
  switch (d->dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      if (message_id.is_server()) {
        unique_message_id = message_id.get_server_message_id();
      }
      //FOR DEBUG
      //text = get_search_text(m);
      //if (!text.empty()) {
      //search_id = (static_cast<int64>(m->date) << 32) | static_cast<uint32>(Random::secure_int32());
      //}
      break;
    case DialogType::Channel:
      break;
    case DialogType::SecretChat:
      random_id = m->random_id;
      text = get_search_text(m);
      if (!text.empty()) {
        search_id = (static_cast<int64>(m->date) << 32) | static_cast<uint32>(m->random_id);
      }
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  int32 ttl_expires_at = 0;
  if (m->ttl_expires_at != 0) {
    ttl_expires_at = static_cast<int32>(m->ttl_expires_at - Time::now() + G()->server_time());
  }
  G()->td_db()->get_messages_db_async()->add_message({d->dialog_id, message_id}, unique_message_id, m->sender_user_id,
                                                     random_id, ttl_expires_at, get_message_index_mask(d->dialog_id, m),
                                                     search_id, text, log_event_store(*m), Auto());  // TODO Promise
}

void MessagesManager::delete_all_dialog_messages_from_database(DialogId dialog_id, MessageId message_id,
                                                               const char *source) {
  if (!G()->parameters().use_message_db) {
    return;
  }

  CHECK(dialog_id.is_valid());
  if (!message_id.is_valid()) {
    return;
  }

  LOG(INFO) << "Delete all messages in " << dialog_id << " from database up to " << message_id << " from " << source;
  /*
  if (dialog_id.get_type() == DialogType::User && message_id.is_server()) {
    bool need_save = false;
    int i = 0;
    for (auto &first_message_id : calls_db_state_.first_calls_database_message_id_by_index) {
      if (first_message_id.get() <= message_id.get()) {
        first_message_id = message_id.get_next_server_message_id();
        calls_db_state_.message_count_by_index[i] = -1;
        need_save = true;
      }
      i++;
    }
    if (need_save) {
      save_calls_db_state();
    }
  }
*/
  G()->td_db()->get_messages_db_async()->delete_all_dialog_messages(dialog_id, message_id, Auto());  // TODO Promise
}

class MessagesManager::DeleteMessageLogEvent {
 public:
  LogEvent::Id id_{0};
  FullMessageId full_message_id_;
  std::vector<FileId> file_ids_;

  template <class StorerT>
  void store(StorerT &storer) const {
    bool has_file_ids = !file_ids_.empty();
    BEGIN_STORE_FLAGS();
    STORE_FLAG(has_file_ids);
    END_STORE_FLAGS();

    td::store(full_message_id_, storer);
    if (has_file_ids) {
      td::store(file_ids_, storer);
    }
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    bool has_file_ids;
    BEGIN_PARSE_FLAGS();
    PARSE_FLAG(has_file_ids);
    END_PARSE_FLAGS();

    td::parse(full_message_id_, parser);
    if (has_file_ids) {
      td::parse(file_ids_, parser);
    }
  }
};

void MessagesManager::delete_message_files(const Message *m) const {
  for (auto file_id : get_message_file_ids(m)) {
    send_closure(G()->file_manager(), &FileManager::delete_file, file_id, Promise<>(),
                 "delete_message_files");  // TODO add log event
  }
}

void MessagesManager::delete_message_from_database(Dialog *d, MessageId message_id, const Message *m,
                                                   bool is_permanently_deleted) const {
  if (is_permanently_deleted) {
    d->deleted_message_ids.insert(message_id);
  }

  if (message_id.is_yet_unsent()) {
    return;
  }

  auto is_secret = d->dialog_id.get_type() == DialogType::SecretChat;
  if (m != nullptr && m->ttl > 0) {
    delete_message_files(m);
  }

  if (!G()->parameters().use_message_db) {
    // TODO message files should be deleted anyway
    // TODO message should be deleted anyway after restart
    return;
  }

  DeleteMessageLogEvent logevent;

  logevent.full_message_id_ = {d->dialog_id, message_id};

  if (is_secret && m != nullptr) {
    logevent.file_ids_ = get_message_file_ids(m);
  }

  do_delete_message_logevent(logevent);
}

void MessagesManager::do_delete_message_logevent(const DeleteMessageLogEvent &logevent) const {
  Promise<Unit> db_promise;
  if (!logevent.file_ids_.empty()) {
    auto logevent_id = logevent.id_;
    if (!logevent_id) {
      auto storer = LogEventStorerImpl<DeleteMessageLogEvent>(logevent);
      logevent_id = BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::DeleteMessage, storer);
    }

    MultiPromiseActorSafe mpas;
    mpas.add_promise(PromiseCreator::lambda([logevent_id](Result<Unit> result) {
      if (result.is_error()) {
        return;
      }
      BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
    }));

    auto lock = mpas.get_promise();
    for (auto file_id : logevent.file_ids_) {
      send_closure(G()->file_manager(), &FileManager::delete_file, file_id, mpas.get_promise(),
                   "do_delete_message_logevent");
    }
    db_promise = mpas.get_promise();
    lock.set_value(Unit());
  }

  // message may not exist in the dialog
  LOG(INFO) << "Delete " << logevent.full_message_id_ << " from database";
  G()->td_db()->get_messages_db_async()->delete_message(logevent.full_message_id_, std::move(db_promise));
}

void MessagesManager::attach_message_to_previous(Dialog *d, MessageId message_id) {
  CHECK(d != nullptr);
  MessagesIterator it(d, message_id);
  Message *message = *it;
  CHECK(message != nullptr);
  CHECK(message->message_id == message_id);
  CHECK(message->have_previous);
  --it;
  CHECK(*it != nullptr);
  LOG(INFO) << "Attach " << message_id << " to the previous " << (*it)->message_id;
  if ((*it)->have_next) {
    message->have_next = true;
  } else {
    (*it)->have_next = true;
  }
}

void MessagesManager::attach_message_to_next(Dialog *d, MessageId message_id) {
  CHECK(d != nullptr);
  MessagesIterator it(d, message_id);
  Message *message = *it;
  CHECK(message != nullptr);
  CHECK(message->message_id == message_id);
  CHECK(message->have_next);
  ++it;
  CHECK(*it != nullptr);
  LOG(INFO) << "Attach " << message_id << " to the next " << (*it)->message_id;
  if ((*it)->have_previous) {
    message->have_previous = true;
  } else {
    (*it)->have_previous = true;
  }
}

void MessagesManager::update_message(Dialog *d, unique_ptr<Message> &old_message, unique_ptr<Message> new_message,
                                     bool need_send_update_message_content, bool *need_update_dialog_pos) {
  CHECK(d != nullptr);
  CHECK(old_message != nullptr);
  CHECK(new_message != nullptr);
  CHECK(old_message->message_id == new_message->message_id);
  CHECK(old_message->random_y == new_message->random_y);
  CHECK(need_update_dialog_pos != nullptr);

  DialogId dialog_id = d->dialog_id;
  MessageId message_id = old_message->message_id;
  bool is_changed = false;
  if (old_message->date != new_message->date) {
    if (new_message->date > 0) {
      LOG_IF(ERROR,
             !new_message->is_outgoing && dialog_id != DialogId(td_->contacts_manager_->get_my_id("update_message")))
          << "Date has changed for incoming " << message_id << " in " << dialog_id << " from " << old_message->date
          << " to " << new_message->date;
      CHECK(old_message->date > 0);
      old_message->date = new_message->date;
      if (d->last_message_id == message_id) {
        *need_update_dialog_pos = true;
      }
      is_changed = true;
    } else {
      LOG(ERROR) << "Receive " << message_id << " in " << dialog_id << " with wrong date " << new_message->date;
    }
  }
  bool is_edited = false;
  if (old_message->edit_date != new_message->edit_date) {
    if (new_message->edit_date > 0) {
      if (new_message->edit_date > old_message->edit_date) {
        old_message->edit_date = new_message->edit_date;
        is_edited = true;
        is_changed = true;
      }
    } else {
      LOG(ERROR) << "Receive " << message_id << " in " << dialog_id << " with wrong edit date "
                 << new_message->edit_date << ", old edit date = " << old_message->edit_date;
    }
  }

  if (old_message->author_signature != new_message->author_signature) {
    LOG_IF(INFO, !old_message->sender_user_id.is_valid() || new_message->sender_user_id.is_valid())
        << "Author signature has changed for " << message_id << " in " << dialog_id << " sent by "
        << old_message->sender_user_id << "/" << new_message->sender_user_id << " from "
        << old_message->author_signature << " to " << new_message->author_signature;
    old_message->author_signature = std::move(new_message->author_signature);
    is_changed = true;
  }
  if (old_message->sender_user_id != new_message->sender_user_id) {
    // there can be race for sent signed posts
    LOG_IF(ERROR, old_message->sender_user_id != UserId() && new_message->sender_user_id != UserId())
        << message_id << " in " << dialog_id << " has changed sender from " << old_message->sender_user_id << " to "
        << new_message->sender_user_id;

    LOG_IF(WARNING, new_message->sender_user_id.is_valid() || old_message->author_signature.empty())
        << "Update message sender from " << old_message->sender_user_id << " to " << new_message->sender_user_id
        << " in " << dialog_id;
    old_message->sender_user_id = new_message->sender_user_id;
    is_changed = true;
  }
  if (old_message->forward_info == nullptr) {
    if (new_message->forward_info != nullptr) {
      LOG(ERROR) << message_id << " in " << dialog_id << " has received forward info " << *new_message->forward_info;
    }
  } else {
    if (new_message->forward_info != nullptr) {
      if (*old_message->forward_info != *new_message->forward_info &&
          (!old_message->forward_info->sender_user_id.is_valid() ||
           new_message->forward_info->sender_user_id.is_valid())) {
        old_message->forward_info->author_signature = new_message->forward_info->author_signature;
        LOG_IF(ERROR, *old_message->forward_info != *new_message->forward_info)
            << message_id << " in " << dialog_id << " has changed forward info from " << *old_message->forward_info
            << " to " << *new_message->forward_info << ", really forwarded from " << old_message->debug_forward_from;
      }
      old_message->forward_info = std::move(new_message->forward_info);
      is_changed = true;
    } else {
      LOG(ERROR) << message_id << " in " << dialog_id << " sent by " << old_message->sender_user_id
                 << " has lost forward info " << *old_message->forward_info << ", really forwarded from "
                 << old_message->debug_forward_from;
      old_message->forward_info = nullptr;
      is_changed = true;
    }
  }

  if (old_message->reply_to_message_id != new_message->reply_to_message_id) {
    if (new_message->reply_to_message_id == MessageId() &&
        get_message_force(d, old_message->reply_to_message_id) == nullptr) {
      old_message->reply_to_message_id = MessageId();
      is_changed = true;
    } else {
      LOG(ERROR) << message_id << " in " << dialog_id << " has changed message it is reply to from "
                 << old_message->reply_to_message_id << " to " << new_message->reply_to_message_id;
      dump_debug_message_op(d);
    }
  }
  LOG_IF(ERROR, old_message->via_bot_user_id != new_message->via_bot_user_id)
      << message_id << " in " << dialog_id << " has changed bot via it is sent from " << old_message->via_bot_user_id
      << " to " << new_message->via_bot_user_id;
  LOG_IF(ERROR, old_message->is_outgoing != new_message->is_outgoing)
      << message_id << " in " << dialog_id << " has changed is_outgoing from " << old_message->is_outgoing << " to "
      << new_message->is_outgoing;
  LOG_IF(ERROR, old_message->is_channel_post != new_message->is_channel_post)
      << message_id << " in " << dialog_id << " has changed is_channel_post from " << old_message->is_channel_post
      << " to " << new_message->is_channel_post;
  LOG_IF(ERROR, old_message->contains_mention != new_message->contains_mention && old_message->edit_date == 0)
      << message_id << " in " << dialog_id << " has changed contains_mention from " << old_message->contains_mention
      << " to " << new_message->contains_mention;
  LOG_IF(ERROR, old_message->disable_notification != new_message->disable_notification && old_message->edit_date == 0)
      << "Disable_notification has changed from " << old_message->disable_notification << " to "
      << new_message->disable_notification
      << ". Old message: " << to_string(get_message_object(dialog_id, old_message.get()))
      << ". New message: " << to_string(get_message_object(dialog_id, new_message.get()));

  if (update_message_contains_unread_mention(d, old_message.get(), new_message->contains_unread_mention,
                                             "update_message")) {
    is_changed = true;
  }
  if (update_message_views(dialog_id, old_message.get(), new_message->views)) {
    is_changed = true;
  }
  if ((old_message->media_album_id == 0 || td_->auth_manager_->is_bot()) && new_message->media_album_id != 0) {
    old_message->media_album_id = new_message->media_album_id;
    is_changed = true;
  }

  if (old_message->edit_date > 0) {
    // inline keyboard can be edited
    bool reply_markup_changed =
        ((old_message->reply_markup == nullptr) != (new_message->reply_markup == nullptr)) ||
        (old_message->reply_markup != nullptr && *old_message->reply_markup != *new_message->reply_markup);
    if (reply_markup_changed) {
      if (d->reply_markup_message_id == message_id && !td_->auth_manager_->is_bot() &&
          new_message->reply_markup == nullptr) {
        set_dialog_reply_markup(d, MessageId());
      }
      old_message->reply_markup = std::move(new_message->reply_markup);
      is_edited = true;
      is_changed = true;
    }
    old_message->had_reply_markup = false;
  } else {
    if (old_message->reply_markup == nullptr) {
      if (new_message->reply_markup != nullptr) {
        auto content_type = old_message->content->get_id();
        // MessageGame and MessageInvoice reply markup can be generated server side
        LOG_IF(ERROR, content_type != MessageGame::ID && content_type != MessageInvoice::ID)
            << message_id << " in " << dialog_id << " has received reply markup " << *new_message->reply_markup;

        old_message->had_reply_markup = false;
        old_message->reply_markup = std::move(new_message->reply_markup);
        is_changed = true;
      }
    } else {
      if (new_message->reply_markup != nullptr) {
        LOG_IF(WARNING, *old_message->reply_markup != *new_message->reply_markup)
            << message_id << " in " << dialog_id << " has changed reply_markup from " << *old_message->reply_markup
            << " to " << *new_message->reply_markup;
      } else {
        LOG(ERROR) << message_id << " in " << dialog_id << " sent by " << old_message->sender_user_id
                   << " has lost reply markup " << *old_message->reply_markup;
      }
    }
  }

  if (old_message->last_access_date < new_message->last_access_date) {
    old_message->last_access_date = new_message->last_access_date;
  }

  CHECK(!new_message->have_previous || !new_message->have_next);
  if (new_message->have_previous && !old_message->have_previous) {
    old_message->have_previous = true;
    attach_message_to_previous(d, message_id);
  } else if (new_message->have_next && !old_message->have_next) {
    old_message->have_next = true;
    attach_message_to_next(d, message_id);
  }

  if (update_message_content(dialog_id, old_message.get(), old_message->content, std::move(new_message->content),
                             need_send_update_message_content)) {
    is_changed = true;
  }
  // TODO update can be send only if the message has already been returned to the user
  if (is_edited && !td_->auth_manager_->is_bot()) {
    send_update_message_edited(dialog_id, old_message.get());
  }

  (void)is_changed;
  // need to save message always, because it might be added to some message index
  // if (is_changed) {
  on_message_changed(d, old_message.get(), "update_message");
  // }
}

bool MessagesManager::need_message_text_changed_warning(const Message *old_message, const MessageText *old_content,
                                                        const MessageText *new_content) {
  if (old_message->edit_date > 0) {
    // message was edited
    return false;
  }
  if (old_message->message_id.is_yet_unsent() && old_message->forward_info != nullptr) {
    // original message may be edited
    return false;
  }
  if (new_content->text.text == "Unsupported characters" ||
      new_content->text.text == "This channel is blocked because it was used to spread pornographic content.") {
    // message contained unsupported characters, text is replaced
    return false;
  }
  if (old_message->message_id.is_yet_unsent() && !old_content->text.entities.empty() &&
      old_content->text.entities[0].offset == 0 &&
      (new_content->text.entities.empty() || new_content->text.entities[0].offset != 0) &&
      old_content->text.text != new_content->text.text && ends_with(old_content->text.text, new_content->text.text)) {
    // server has deleted first entity and ltrim the message
    return false;
  }
  for (auto &entity : new_content->text.entities) {
    if (entity.type == MessageEntity::Type::PhoneNumber) {
      // TODO remove after find_phone_numbers is implemented
      return false;
    }
  }
  return true;
}

bool MessagesManager::update_message_content(DialogId dialog_id, Message *old_message,
                                             unique_ptr<MessageContent> &old_content,
                                             unique_ptr<MessageContent> new_content,
                                             bool need_send_update_message_content) {
  bool is_content_changed = false;
  bool need_update = false;
  int32 old_content_type = old_content->get_id();
  int32 new_content_type = new_content->get_id();
  bool can_delete_old_document = old_message->message_id.is_yet_unsent() && false;

  if (old_content_type != new_content_type) {
    need_update = true;
    LOG(INFO) << "Message content has changed its type from " << old_content_type << " to " << new_content_type;

    old_message->is_content_secret = is_secret_message_content(old_message->ttl, new_content->get_id());

    auto old_file_id = get_message_content_file_id(old_content.get());
    if (old_file_id.is_valid()) {
      // cancel file upload of the main file to allow next upload with the same file to succeed
      td_->file_manager_->upload(old_file_id, nullptr, 0, 0);

      auto new_file_id = get_message_content_file_id(new_content.get());
      if (new_file_id.is_valid()) {
        FileView old_file_view = td_->file_manager_->get_file_view(old_file_id);
        FileView new_file_view = td_->file_manager_->get_file_view(new_file_id);
        // if file type has changed, but file size remains the same, we are trying to update local location of the new
        // file with the old local location
        if (old_file_view.has_local_location() && !new_file_view.has_local_location() && old_file_view.size() != 0 &&
            old_file_view.size() == new_file_view.size()) {
          auto old_file_type = old_file_view.get_type();
          auto new_file_type = new_file_view.get_type();
          auto is_document_file_type = [](FileType file_type) {
            switch (file_type) {
              case FileType::Video:
              case FileType::VoiceNote:
              case FileType::Document:
              case FileType::Sticker:
              case FileType::Audio:
              case FileType::Animation:
              case FileType::VideoNote:
                return true;
              default:
                return false;
            }
          };

          if (is_document_file_type(old_file_type) && is_document_file_type(new_file_type)) {
            auto &old_location = old_file_view.local_location();
            auto r_file_id = td_->file_manager_->register_local(
                FullLocalFileLocation(new_file_type, old_location.path_, old_location.mtime_nsec_), dialog_id,
                old_file_view.size());
            if (r_file_id.is_ok()) {
              LOG_STATUS(td_->file_manager_->merge(new_file_id, r_file_id.ok()));
            }
          }
        }
      }
    }
  } else {
    switch (new_content_type) {
      case MessageText::ID: {
        auto old_ = static_cast<const MessageText *>(old_content.get());
        auto new_ = static_cast<const MessageText *>(new_content.get());
        if (old_->text.text != new_->text.text) {
          if (need_message_text_changed_warning(old_message, old_, new_)) {
            LOG(ERROR) << "Message text has changed for " << to_string(get_message_object(dialog_id, old_message))
                       << ". New content is "
                       << to_string(get_message_content_object(new_content.get(), old_message->date,
                                                               old_message->is_content_secret));
          }
          need_update = true;
        }
        if (old_->text.entities != new_->text.entities) {
          const int32 MAX_CUSTOM_ENTITIES_COUNT = 100;  // server-size limit
          if (need_message_text_changed_warning(old_message, old_, new_) &&
              old_->text.entities.size() <= MAX_CUSTOM_ENTITIES_COUNT) {
            LOG(WARNING) << "Entities has changed for " << to_string(get_message_object(dialog_id, old_message))
                         << ". New content is "
                         << to_string(get_message_content_object(new_content.get(), old_message->date,
                                                                 old_message->is_content_secret));
          }
          need_update = true;
        }
        if (old_->web_page_id != new_->web_page_id) {
          LOG(INFO) << "Old: " << old_->web_page_id << ", new: " << new_->web_page_id;
          is_content_changed = true;
          need_update |= td_->web_pages_manager_->have_web_page(old_->web_page_id) ||
                         td_->web_pages_manager_->have_web_page(new_->web_page_id);
        }
        break;
      }
      case MessageAnimation::ID: {
        auto old_ = static_cast<const MessageAnimation *>(old_content.get());
        auto new_ = static_cast<const MessageAnimation *>(new_content.get());
        if (td_->animations_manager_->merge_animations(new_->file_id, old_->file_id, can_delete_old_document)) {
          need_update = true;
        }
        if (old_->caption != new_->caption) {
          need_update = true;
        }
        break;
      }
      case MessageAudio::ID: {
        auto old_ = static_cast<const MessageAudio *>(old_content.get());
        auto new_ = static_cast<const MessageAudio *>(new_content.get());
        if (td_->audios_manager_->merge_audios(new_->file_id, old_->file_id, can_delete_old_document)) {
          need_update = true;
        }
        if (old_->caption != new_->caption) {
          need_update = true;
        }
        break;
      }
      case MessageContact::ID: {
        auto old_ = static_cast<const MessageContact *>(old_content.get());
        auto new_ = static_cast<const MessageContact *>(new_content.get());
        if (old_->contact != new_->contact) {
          need_update = true;
        }
        break;
      }
      case MessageDocument::ID: {
        auto old_ = static_cast<const MessageDocument *>(old_content.get());
        auto new_ = static_cast<const MessageDocument *>(new_content.get());
        if (td_->documents_manager_->merge_documents(new_->file_id, old_->file_id, can_delete_old_document)) {
          need_update = true;
        }
        if (old_->caption != new_->caption) {
          need_update = true;
        }
        break;
      }
      case MessageGame::ID: {
        auto old_ = static_cast<const MessageGame *>(old_content.get());
        auto new_ = static_cast<const MessageGame *>(new_content.get());
        if (old_->game != new_->game) {
          need_update = true;
        }
        break;
      }
      case MessageInvoice::ID: {
        auto old_ = static_cast<const MessageInvoice *>(old_content.get());
        auto new_ = static_cast<const MessageInvoice *>(new_content.get());
        if (old_->title != new_->title || old_->description != new_->description || old_->photo != new_->photo ||
            old_->start_parameter != new_->start_parameter || old_->invoice != new_->invoice ||
            old_->total_amount != new_->total_amount || old_->receipt_message_id != new_->receipt_message_id) {
          need_update = true;
        }
        if (old_->payload != new_->payload || old_->provider_token != new_->provider_token ||
            old_->provider_data != new_->provider_data) {
          is_content_changed = true;
        }
        break;
      }
      case MessageLiveLocation::ID: {
        auto old_ = static_cast<const MessageLiveLocation *>(old_content.get());
        auto new_ = static_cast<const MessageLiveLocation *>(new_content.get());
        if (old_->location != new_->location) {
          need_update = true;
        }
        if (old_->period != new_->period) {
          need_update = true;
        }
        break;
      }
      case MessageLocation::ID: {
        auto old_ = static_cast<const MessageLocation *>(old_content.get());
        auto new_ = static_cast<const MessageLocation *>(new_content.get());
        if (old_->location != new_->location) {
          need_update = true;
        }
        break;
      }
      case MessagePhoto::ID: {
        auto old_ = static_cast<const MessagePhoto *>(old_content.get());
        auto new_ = static_cast<MessagePhoto *>(new_content.get());
        const Photo *old_photo = &old_->photo;
        Photo *new_photo = &new_->photo;
        if (old_photo->date != new_photo->date) {
          is_content_changed = true;
        }
        if (old_photo->id != new_photo->id || old_->caption != new_->caption) {
          need_update = true;
        }
        if (old_photo->photos != new_photo->photos) {
          if ((old_photo->photos.size() == 1 || (old_photo->photos.size() == 2 && old_photo->photos[0].type == 't')) &&
              old_photo->photos.back().type == 'i' && new_photo->photos.size() > 0) {
            // first time get info about sent photo
            if (old_photo->photos.size() == 2) {
              new_photo->photos.push_back(old_photo->photos[0]);
            }
            new_photo->photos.push_back(old_photo->photos.back());
            FileId old_file_id = old_photo->photos.back().file_id;
            FileView old_file_view = td_->file_manager_->get_file_view(old_file_id);
            FileId new_file_id = new_photo->photos[0].file_id;
            FileView new_file_view = td_->file_manager_->get_file_view(new_file_id);
            if (!old_file_view.has_remote_location()) {
              CHECK(new_file_view.has_remote_location());
              CHECK(!new_file_view.remote_location().is_web());
              FileId file_id = td_->file_manager_->register_remote(
                  FullRemoteFileLocation(FileType::Photo, new_file_view.remote_location().get_id(),
                                         new_file_view.remote_location().get_access_hash(), 0, 0, 0, DcId::invalid()),
                  FileLocationSource::FromServer, dialog_id, old_photo->photos.back().size, 0, "");
              LOG_STATUS(td_->file_manager_->merge(file_id, old_file_id));
            }
          }
          if ((old_photo->photos.size() == 1 + new_photo->photos.size() ||
               (old_photo->photos.size() == 2 + new_photo->photos.size() &&
                old_photo->photos[new_photo->photos.size()].type == 't')) &&
              old_photo->photos.back().type == 'i') {
            // get sent photo again
            if (old_photo->photos.size() == 2 + new_photo->photos.size()) {
              new_photo->photos.push_back(old_photo->photos[new_photo->photos.size()]);
            }
            new_photo->photos.push_back(old_photo->photos.back());
          }
          if (old_photo->photos != new_photo->photos) {
            need_update = true;
          }
        }
        break;
      }
      case MessageSticker::ID: {
        auto old_ = static_cast<const MessageSticker *>(old_content.get());
        auto new_ = static_cast<const MessageSticker *>(new_content.get());
        if (td_->stickers_manager_->merge_stickers(new_->file_id, old_->file_id, can_delete_old_document)) {
          need_update = true;
        }
        break;
      }
      case MessageVenue::ID: {
        auto old_ = static_cast<const MessageVenue *>(old_content.get());
        auto new_ = static_cast<const MessageVenue *>(new_content.get());
        if (old_->venue != new_->venue) {
          need_update = true;
        }
        break;
      }
      case MessageVideo::ID: {
        auto old_ = static_cast<const MessageVideo *>(old_content.get());
        auto new_ = static_cast<const MessageVideo *>(new_content.get());
        if (td_->videos_manager_->merge_videos(new_->file_id, old_->file_id, can_delete_old_document)) {
          need_update = true;
        }
        if (old_->caption != new_->caption) {
          need_update = true;
        }
        break;
      }
      case MessageVideoNote::ID: {
        auto old_ = static_cast<const MessageVideoNote *>(old_content.get());
        auto new_ = static_cast<const MessageVideoNote *>(new_content.get());
        if (td_->video_notes_manager_->merge_video_notes(new_->file_id, old_->file_id, can_delete_old_document)) {
          need_update = true;
        }
        if (old_->is_viewed != new_->is_viewed) {
          need_update = true;
        }
        break;
      }
      case MessageVoiceNote::ID: {
        auto old_ = static_cast<const MessageVoiceNote *>(old_content.get());
        auto new_ = static_cast<const MessageVoiceNote *>(new_content.get());
        if (td_->voice_notes_manager_->merge_voice_notes(new_->file_id, old_->file_id, can_delete_old_document)) {
          need_update = true;
        }
        if (old_->caption != new_->caption) {
          need_update = true;
        }
        if (old_->is_listened != new_->is_listened) {
          need_update = true;
        }
        break;
      }
      case MessageChatCreate::ID: {
        auto old_ = static_cast<const MessageChatCreate *>(old_content.get());
        auto new_ = static_cast<const MessageChatCreate *>(new_content.get());
        if (old_->title != new_->title || old_->participant_user_ids != new_->participant_user_ids) {
          need_update = true;
        }
        break;
      }
      case MessageChatChangeTitle::ID: {
        auto old_ = static_cast<const MessageChatChangeTitle *>(old_content.get());
        auto new_ = static_cast<const MessageChatChangeTitle *>(new_content.get());
        if (old_->title != new_->title) {
          need_update = true;
        }
        break;
      }
      case MessageChatChangePhoto::ID: {
        auto old_ = static_cast<const MessageChatChangePhoto *>(old_content.get());
        auto new_ = static_cast<const MessageChatChangePhoto *>(new_content.get());
        if (old_->photo != new_->photo) {
          need_update = true;
        }
        break;
      }
      case MessageChatDeletePhoto::ID:
        break;
      case MessageChatDeleteHistory::ID:
        break;
      case MessageChatAddUsers::ID: {
        auto old_ = static_cast<const MessageChatAddUsers *>(old_content.get());
        auto new_ = static_cast<const MessageChatAddUsers *>(new_content.get());
        if (old_->user_ids != new_->user_ids) {
          need_update = true;
        }
        break;
      }
      case MessageChatJoinedByLink::ID:
        break;
      case MessageChatDeleteUser::ID: {
        auto old_ = static_cast<const MessageChatDeleteUser *>(old_content.get());
        auto new_ = static_cast<const MessageChatDeleteUser *>(new_content.get());
        if (old_->user_id != new_->user_id) {
          need_update = true;
        }
        break;
      }
      case MessageChatMigrateTo::ID: {
        auto old_ = static_cast<const MessageChatMigrateTo *>(old_content.get());
        auto new_ = static_cast<const MessageChatMigrateTo *>(new_content.get());
        if (old_->migrated_to_channel_id != new_->migrated_to_channel_id) {
          need_update = true;
        }
        break;
      }
      case MessageChannelCreate::ID: {
        auto old_ = static_cast<const MessageChannelCreate *>(old_content.get());
        auto new_ = static_cast<const MessageChannelCreate *>(new_content.get());
        if (old_->title != new_->title) {
          need_update = true;
        }
        break;
      }
      case MessageChannelMigrateFrom::ID: {
        auto old_ = static_cast<const MessageChannelMigrateFrom *>(old_content.get());
        auto new_ = static_cast<const MessageChannelMigrateFrom *>(new_content.get());
        if (old_->title != new_->title || old_->migrated_from_chat_id != new_->migrated_from_chat_id) {
          need_update = true;
        }
        break;
      }
      case MessagePinMessage::ID: {
        auto old_ = static_cast<const MessagePinMessage *>(old_content.get());
        auto new_ = static_cast<const MessagePinMessage *>(new_content.get());
        if (old_->message_id != new_->message_id) {
          need_update = true;
        }
        break;
      }
      case MessageGameScore::ID: {
        auto old_ = static_cast<const MessageGameScore *>(old_content.get());
        auto new_ = static_cast<const MessageGameScore *>(new_content.get());
        if (old_->game_message_id != new_->game_message_id || old_->game_id != new_->game_id ||
            old_->score != new_->score) {
          need_update = true;
        }
        break;
      }
      case MessageScreenshotTaken::ID:
        break;
      case MessageChatSetTtl::ID: {
        auto old_ = static_cast<const MessageChatSetTtl *>(old_content.get());
        auto new_ = static_cast<const MessageChatSetTtl *>(new_content.get());
        if (old_->ttl != new_->ttl) {
          LOG(ERROR) << "Ttl has changed from " << old_->ttl << " to " << new_->ttl;
          need_update = true;
        }
        break;
      }
      case MessageCall::ID: {
        auto old_ = static_cast<const MessageCall *>(old_content.get());
        auto new_ = static_cast<const MessageCall *>(new_content.get());
        if (old_->call_id != new_->call_id) {
          is_content_changed = true;
        }
        if (old_->duration != new_->duration || old_->discard_reason != new_->discard_reason) {
          need_update = true;
        }
        break;
      }
      case MessagePaymentSuccessful::ID: {
        auto old_ = static_cast<const MessagePaymentSuccessful *>(old_content.get());
        auto new_ = static_cast<const MessagePaymentSuccessful *>(new_content.get());
        if (old_->invoice_message_id != new_->invoice_message_id || old_->currency != new_->currency ||
            old_->total_amount != new_->total_amount || old_->invoice_payload != new_->invoice_payload ||
            old_->shipping_option_id != new_->shipping_option_id ||
            old_->telegram_payment_charge_id != new_->telegram_payment_charge_id ||
            old_->provider_payment_charge_id != new_->provider_payment_charge_id ||
            ((old_->order_info != nullptr || new_->order_info != nullptr) &&
             (old_->order_info == nullptr || new_->order_info == nullptr || *old_->order_info != *new_->order_info))) {
          need_update = true;
        }
        break;
      }
      case MessageContactRegistered::ID:
        break;
      case MessageExpiredPhoto::ID:
        break;
      case MessageExpiredVideo::ID:
        break;
      case MessageCustomServiceAction::ID: {
        auto old_ = static_cast<const MessageCustomServiceAction *>(old_content.get());
        auto new_ = static_cast<const MessageCustomServiceAction *>(new_content.get());
        if (old_->message != new_->message) {
          need_update = true;
        }
        break;
      }
      case MessageWebsiteConnected::ID: {
        auto old_ = static_cast<const MessageWebsiteConnected *>(old_content.get());
        auto new_ = static_cast<const MessageWebsiteConnected *>(new_content.get());
        if (old_->domain_name != new_->domain_name) {
          need_update = true;
        }
        break;
      }
      case MessageUnsupported::ID:
        break;
      default:
        UNREACHABLE();
        break;
    }
  }

  if (is_content_changed || need_update) {
    auto old_file_id = get_message_content_file_id(old_content.get());
    old_content = std::move(new_content);
    update_message_content_file_id_remote(old_content.get(), old_file_id);
  } else {
    update_message_content_file_id_remote(old_content.get(), get_message_content_file_id(new_content.get()));
  }
  if (is_content_changed && !need_update) {
    LOG(INFO) << "Content of " << old_message->message_id << " in " << dialog_id << " has changed";
  }

  if (need_update && need_send_update_message_content) {
    send_update_message_content(dialog_id, old_message->message_id, old_content.get(), old_message->date,
                                old_message->is_content_secret, "update_message_content");
  }
  return is_content_changed || need_update;
}

MessagesManager::Dialog *MessagesManager::get_dialog_by_message_id(MessageId message_id) {
  CHECK(message_id.is_valid() && message_id.is_server());
  auto it = message_id_to_dialog_id_.find(message_id);
  if (it == message_id_to_dialog_id_.end()) {
    if (G()->parameters().use_message_db) {
      auto r_value =
          G()->td_db()->get_messages_db_sync()->get_message_by_unique_message_id(message_id.get_server_message_id());
      if (r_value.is_ok()) {
        DialogId dialog_id(r_value.ok().first);
        Message *m = on_get_message_from_database(dialog_id, get_dialog_force(dialog_id), r_value.ok().second);
        if (m != nullptr) {
          CHECK(m->message_id == message_id);
          CHECK(message_id_to_dialog_id_[message_id] == dialog_id);
          Dialog *d = get_dialog(dialog_id);
          CHECK(d != nullptr);
          return d;
        }
      }
    }

    return nullptr;
  }

  return get_dialog(it->second);
}

MessageId MessagesManager::get_message_id_by_random_id(Dialog *d, int64 random_id) {
  CHECK(d != nullptr);
  CHECK(d->dialog_id.get_type() == DialogType::SecretChat);
  if (random_id == 0) {
    return MessageId();
  }
  auto it = d->random_id_to_message_id.find(random_id);
  if (it == d->random_id_to_message_id.end()) {
    if (G()->parameters().use_message_db) {
      auto r_value = G()->td_db()->get_messages_db_sync()->get_message_by_random_id(d->dialog_id, random_id);
      if (r_value.is_ok()) {
        Message *m = on_get_message_from_database(d->dialog_id, d, r_value.ok());
        if (m != nullptr) {
          CHECK(m->random_id == random_id);
          CHECK(d->random_id_to_message_id[random_id] == m->message_id);
          return m->message_id;
        }
      }
    }

    return MessageId();
  }

  return it->second;
}

void MessagesManager::force_create_dialog(DialogId dialog_id, const char *source, bool force_update_dialog_pos) {
  CHECK(dialog_id.is_valid());
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(INFO) << "Force create " << dialog_id << " from " << source;
    if (loaded_dialogs_.count(dialog_id) > 0) {
      return;
    }

    d = add_dialog(dialog_id);
    update_dialog_pos(d, false, "force_create_dialog");

    if (have_input_peer(dialog_id, AccessRights::Read)) {
      if (dialog_id.get_type() != DialogType::SecretChat && !d->notification_settings.is_synchronized) {
        // asynchronously preload information about the dialog
        send_get_dialog_query(dialog_id, Auto());
      }
    } else {
      if (!have_dialog_info(dialog_id)) {
        LOG(ERROR) << "Have no info about " << dialog_id << " received from " << source << ", but forced to create it";
      } else {
        LOG_IF(ERROR, Slice(source) != Slice("message forward info") &&
                          Slice(source) != Slice("on_new_callback_query") &&
                          Slice(source) != Slice("search public dialog") &&
                          Slice(source) != Slice("create new secret chat") && !force_update_dialog_pos)
            << "Have no access to " << dialog_id << " received from " << source << ", but forced to create it";
      }
    }
  } else if (force_update_dialog_pos) {
    update_dialog_pos(d, false, "force update dialog pos");
  }
}

MessagesManager::Dialog *MessagesManager::add_dialog(DialogId dialog_id) {
  LOG(DEBUG) << "Creating " << dialog_id;
  CHECK(!have_dialog(dialog_id));

  if (G()->parameters().use_message_db) {
    // TODO preload dialog asynchronously, remove loading from this function
    LOG(INFO) << "Synchronously load " << dialog_id << " from database";
    auto r_value = G()->td_db()->get_dialog_db_sync()->get_dialog(dialog_id);
    if (r_value.is_ok()) {
      return add_new_dialog(parse_dialog(dialog_id, r_value.ok()), true);
    }
  }

  auto d = make_unique<Dialog>();
  std::fill(d->message_count_by_index.begin(), d->message_count_by_index.end(), -1);
  d->dialog_id = dialog_id;

  return add_new_dialog(std::move(d), false);
}

MessagesManager::Dialog *MessagesManager::add_new_dialog(unique_ptr<Dialog> &&d, bool is_loaded_from_database) {
  auto dialog_id = d->dialog_id;
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      break;
    case DialogType::Channel: {
      auto channel_type = td_->contacts_manager_->get_channel_type(dialog_id.get_channel_id());
      if (channel_type == ChannelType::Broadcast) {
        d->last_read_outbox_message_id = MessageId::max();
        d->is_last_read_outbox_message_id_inited = true;
      }
      if (!d->notification_settings.is_synchronized && channel_type == ChannelType::Megagroup) {
        d->notification_settings.mute_until = std::numeric_limits<int32>::max();
      }

      auto pts = load_channel_pts(dialog_id);
      if (pts > 0) {
        d->pts = pts;
        if (is_debug_message_op_enabled()) {
          d->debug_message_op.emplace_back(Dialog::MessageOp::SetPts, MessageId(), pts, false, false, false,
                                           "add_new_dialog");
        }
      }
      break;
    }
    case DialogType::SecretChat:
      if (!d->last_new_message_id.is_valid()) {
        LOG(INFO) << "Set " << d->dialog_id << " last new message id in add_new_dialog";
        // TODO use date << MessageId::SERVER_ID_SHIFT;
        d->last_new_message_id = MessageId::min();
      }
      for (auto &first_message_id : d->first_database_message_id_by_index) {
        first_message_id = MessageId::min();
      }
      for (auto &message_count : d->message_count_by_index) {
        if (message_count == -1) {
          message_count = 0;
        }
      }

      d->have_full_history = true;
      d->need_restore_reply_markup = false;
      d->notification_settings.is_synchronized = true;
      d->know_can_report_spam = true;
      if (!is_loaded_from_database) {
        d->can_report_spam =
            td_->contacts_manager_->default_can_report_spam_in_secret_chat(dialog_id.get_secret_chat_id());
      }
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }
  if (!is_loaded_from_database) {
    on_dialog_updated(dialog_id, "add_new_dialog");
  }

  unique_ptr<Message> last_database_message = std::move(d->messages);
  int64 order = d->order;
  d->order = DEFAULT_ORDER;
  int32 last_clear_history_date = d->last_clear_history_date;
  MessageId last_clear_history_message_id = d->last_clear_history_message_id;
  d->last_clear_history_date = 0;
  d->last_clear_history_message_id = MessageId();

  if (!is_loaded_from_database) {
    CHECK(order == DEFAULT_ORDER);
    CHECK(last_database_message == nullptr);
  }

  auto dialog_it = dialogs_.emplace(dialog_id, std::move(d)).first;

  loaded_dialogs_.erase(dialog_id);

  Dialog *dialog = dialog_it->second.get();
  send_update_chat(dialog);

  fix_new_dialog(dialog, std::move(last_database_message), order, last_clear_history_date,
                 last_clear_history_message_id);

  return dialog;
}

void MessagesManager::fix_new_dialog(Dialog *d, unique_ptr<Message> &&last_database_message, int64 order,
                                     int32 last_clear_history_date, MessageId last_clear_history_message_id) {
  CHECK(d != nullptr);
  auto dialog_id = d->dialog_id;

  if (d->notification_settings.mute_until <= G()->unix_time()) {
    d->notification_settings.mute_until = 0;
  } else {
    update_dialog_unmute_timeout(d, -1, d->notification_settings.mute_until);
  }

  auto pending_it = pending_add_dialog_last_database_message_dependent_dialogs_.find(dialog_id);
  if (pending_it != pending_add_dialog_last_database_message_dependent_dialogs_.end()) {
    auto pending_dialogs = std::move(pending_it->second);
    pending_add_dialog_last_database_message_dependent_dialogs_.erase(pending_it);

    for (auto &pending_dialog_id : pending_dialogs) {
      auto &counter_message = pending_add_dialog_last_database_message_[pending_dialog_id];
      CHECK(counter_message.first > 0);
      counter_message.first--;
      if (counter_message.first == 0) {
        add_dialog_last_database_message(get_dialog(pending_dialog_id), std::move(counter_message.second));
        pending_add_dialog_last_database_message_.erase(pending_dialog_id);
      }
    }
  }

  set_dialog_last_clear_history_date(d, last_clear_history_date, last_clear_history_message_id, "add_new_dialog");

  set_dialog_order(d, order, false);

  if (dialog_id.get_type() != DialogType::SecretChat && d->last_new_message_id.is_valid() &&
      !d->last_new_message_id.is_server()) {
    // fix wrong last_new_message_id
    d->last_new_message_id = MessageId(d->last_new_message_id.get() & ~MessageId::FULL_TYPE_MASK);
  }

  // add last database message to dialog
  if (last_database_message != nullptr) {
    auto message_id = last_database_message->message_id;
    if (!d->first_database_message_id.is_valid()) {
      LOG(ERROR) << "Bugfixing wrong first_database_message_id to " << message_id << " in " << dialog_id;
      set_dialog_first_database_message_id(d, message_id, "add_new_dialog");
    }
    set_dialog_last_database_message_id(d, message_id, "add_new_dialog");
    if ((message_id.is_server() || dialog_id.get_type() == DialogType::SecretChat) &&
        !d->last_new_message_id.is_valid()) {
      // is it even possible?
      LOG(ERROR) << "Bugfixing wrong last_new_message_id to " << message_id << " in " << dialog_id;
      set_dialog_last_new_message_id(d, message_id, "add_new_dialog");
    }

    int32 dependent_dialog_count = 0;
    if (last_database_message->forward_info != nullptr) {
      auto other_dialog_id = last_database_message->forward_info->dialog_id;
      if (other_dialog_id.is_valid() && !have_dialog(other_dialog_id)) {
        LOG(INFO) << "Postpone adding of last message in " << dialog_id << " because of cyclic dependency with "
                  << other_dialog_id;
        pending_add_dialog_last_database_message_dependent_dialogs_[other_dialog_id].push_back(dialog_id);
        dependent_dialog_count++;
      }
      other_dialog_id = last_database_message->forward_info->from_dialog_id;
      if (other_dialog_id.is_valid() && !have_dialog(other_dialog_id)) {
        LOG(INFO) << "Postpone adding of last message in " << dialog_id << " because of cyclic dependency with "
                  << other_dialog_id;
        pending_add_dialog_last_database_message_dependent_dialogs_[other_dialog_id].push_back(dialog_id);
        dependent_dialog_count++;
      }
    }

    if (dependent_dialog_count == 0) {
      add_dialog_last_database_message(d, std::move(last_database_message));
    } else {
      // can't add message immediately, because needs to notify first about adding of dependent dialogs
      pending_add_dialog_last_database_message_[dialog_id] = {dependent_dialog_count, std::move(last_database_message)};
    }
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      break;
    case DialogType::Chat:
      if (d->last_read_inbox_message_id.get() < d->last_read_outbox_message_id.get()) {
        LOG(INFO) << "Have last read outbox message " << d->last_read_outbox_message_id << " in " << dialog_id
                  << ", but last read inbox message is " << d->last_read_inbox_message_id;
        // can't fix last_read_inbox_message_id by last_read_outbox_message_id because last_read_outbox_message_id is
        // just a message id not less than last read outgoing message and less than first unread outgoing message, so
        // it may not point to the outgoing message
        // read_history_inbox(dialog_id, d->last_read_outbox_message_id, d->server_unread_count, "add_new_dialog");
      }
      break;
    case DialogType::Channel:
      break;
    case DialogType::SecretChat:
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }

  update_dialogs_hints(d);

  bool need_get_history = false;
  if (d->delete_last_message_date != 0) {
    if (d->last_message_id.is_valid()) {
      LOG(ERROR) << "Last " << d->deleted_last_message_id << " in " << dialog_id << " was deleted at "
                 << d->delete_last_message_date << ", but have last " << d->last_message_id;
      d->delete_last_message_date = 0;
      d->deleted_last_message_id = MessageId();
      d->is_last_message_deleted_locally = false;
      on_dialog_updated(dialog_id, "update_delete_last_message_date");
    } else {
      need_get_history = true;
    }
  }
  if (!d->last_database_message_id.is_valid()) {
    need_get_history = true;
  }

  if (need_get_history && !td_->auth_manager_->is_bot() && have_input_peer(dialog_id, AccessRights::Read) &&
      d->order != DEFAULT_ORDER) {
    get_history_from_the_end(dialog_id, true, false, Auto());
  }
}

void MessagesManager::add_dialog_last_database_message(Dialog *d, unique_ptr<Message> &&last_database_message) {
  CHECK(d != nullptr);
  CHECK(last_database_message != nullptr);
  CHECK(last_database_message->left == nullptr);
  CHECK(last_database_message->right == nullptr);

  auto message_id = last_database_message->message_id;
  CHECK(d->last_database_message_id == message_id) << message_id << " " << d->last_database_message_id;

  if (!have_input_peer(d->dialog_id, AccessRights::Read)) {
    // do not add last message to inaccessible dialog
    return;
  }

  bool need_update = false;
  bool need_update_dialog_pos = false;
  last_database_message->have_previous = false;
  last_database_message->have_next = false;
  last_database_message->from_database = true;
  Message *m = add_message_to_dialog(d, std::move(last_database_message), false, &need_update, &need_update_dialog_pos,
                                     "add_dialog_last_database_message");
  if (m != nullptr) {
    set_dialog_last_message_id(d, message_id, "add_new_dialog");
    send_update_chat_last_message(d, "add_new_dialog");
  }

  if (need_update_dialog_pos) {
    LOG(ERROR) << "Update pos in " << d->dialog_id;
    update_dialog_pos(d, false, "add_new_dialog");
  }
}

void MessagesManager::update_dialogs_hints(const Dialog *d) {
  if (!td_->auth_manager_->is_bot() && d->order != DEFAULT_ORDER) {
    dialogs_hints_.add(-d->dialog_id.get(), get_dialog_title(d->dialog_id) + ' ' + get_dialog_username(d->dialog_id));
  }
}

void MessagesManager::update_dialogs_hints_rating(const Dialog *d) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  if (d->order == DEFAULT_ORDER) {
    dialogs_hints_.remove(-d->dialog_id.get());
  } else {
    dialogs_hints_.set_rating(-d->dialog_id.get(), -d->order);
  }
}

int64 MessagesManager::get_dialog_order(MessageId message_id, int32 message_date) {
  return (static_cast<int64>(message_date) << 32) + narrow_cast<int32>(message_id.get() >> MessageId::SERVER_ID_SHIFT);
}

int64 MessagesManager::get_next_pinned_dialog_order() {
  if (current_pinned_dialog_order_ == DEFAULT_ORDER) {
    string res_str = G()->td_db()->get_binlog_pmc()->get("dialog_pinned_current_order");
    if (res_str.empty()) {
      current_pinned_dialog_order_ = static_cast<int64>(MIN_PINNED_DIALOG_DATE) << 32;
    } else {
      current_pinned_dialog_order_ = to_integer<int64>(res_str);
    }
  }
  CHECK(current_pinned_dialog_order_ != DEFAULT_ORDER);

  current_pinned_dialog_order_++;
  G()->td_db()->get_binlog_pmc()->set("dialog_pinned_current_order", to_string(current_pinned_dialog_order_));
  LOG(INFO) << "Assign pinned_order = " << current_pinned_dialog_order_;
  return current_pinned_dialog_order_;
}

void MessagesManager::update_dialog_pos(Dialog *d, bool remove_from_dialog_list, const char *source,
                                        bool need_send_update_chat_order) {
  CHECK(d != nullptr);
  LOG(INFO) << "Trying to update " << d->dialog_id << " order from " << source;
  auto dialog_type = d->dialog_id.get_type();

  switch (dialog_type) {
    case DialogType::User:
      break;
    case DialogType::Chat: {
      auto chat_id = d->dialog_id.get_chat_id();
      if (!td_->contacts_manager_->get_chat_is_active(chat_id)) {
        remove_from_dialog_list = true;
      }
      break;
    }
    case DialogType::Channel: {
      auto channel_id = d->dialog_id.get_channel_id();
      auto status = td_->contacts_manager_->get_channel_status(channel_id);
      if (!status.is_member()) {
        remove_from_dialog_list = true;
      }
      break;
    }
    case DialogType::SecretChat:
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
      return;
  }

  int64 new_order = DEFAULT_ORDER;
  if (!remove_from_dialog_list) {
    if (d->pinned_order != DEFAULT_ORDER) {
      LOG(INFO) << "Pin at " << d->pinned_order << " found";
      new_order = d->pinned_order;
    }
    if (d->last_message_id != MessageId()) {
      auto m = get_message(d, d->last_message_id);
      CHECK(m != nullptr);
      LOG(INFO) << "Last message at " << m->date << " found";
      int64 last_message_order = get_dialog_order(m->message_id, m->date);
      if (last_message_order > new_order) {
        new_order = last_message_order;
      }
    } else if (d->delete_last_message_date > 0) {
      LOG(INFO) << "Deleted last " << d->deleted_last_message_id << " at " << d->delete_last_message_date << " found";
      int64 delete_order = get_dialog_order(d->deleted_last_message_id, d->delete_last_message_date);
      if (delete_order > new_order) {
        new_order = delete_order;
      }
    } else if (d->last_clear_history_date > 0) {
      LOG(INFO) << "Clear history at " << d->last_clear_history_date << " found";
      int64 clear_order = get_dialog_order(d->last_clear_history_message_id, d->last_clear_history_date);
      if (clear_order > new_order) {
        new_order = clear_order;
      }
    }
    if (d->draft_message != nullptr) {
      LOG(INFO) << "Draft message at " << d->draft_message->date << " found";
      int64 draft_order = get_dialog_order(MessageId(), d->draft_message->date);
      if (draft_order > new_order) {
        new_order = draft_order;
      }
    }
    if (dialog_type == DialogType::Channel) {
      auto date = td_->contacts_manager_->get_channel_date(d->dialog_id.get_channel_id());
      LOG(INFO) << "Join of channel at " << date << " found";
      int64 join_order = get_dialog_order(MessageId(), date);
      if (join_order > new_order) {
        new_order = join_order;
      }
    }
    if (dialog_type == DialogType::SecretChat) {
      auto secret_chat_id = d->dialog_id.get_secret_chat_id();
      auto date = td_->contacts_manager_->get_secret_chat_date(secret_chat_id);
      auto state = td_->contacts_manager_->get_secret_chat_state(secret_chat_id);
      // do not return removed from the chat list closed secret chats
      if (date != 0 && (d->order != DEFAULT_ORDER || state != SecretChatState::Closed)) {
        LOG(INFO) << "Creation of secret chat at " << date << " found";
        int64 creation_order = get_dialog_order(MessageId(), date);
        if (creation_order > new_order) {
          new_order = creation_order;
        }
      }
    }
    if (new_order == DEFAULT_ORDER && !d->is_empty) {
      // if there is no known messages in the dialog, just leave it where it is
      LOG(INFO) << "There is no known messages in the dialog";
      return;
    }
  }

  if (set_dialog_order(d, new_order, need_send_update_chat_order)) {
    on_dialog_updated(d->dialog_id, "update_dialog_pos");
  }
}

bool MessagesManager::set_dialog_order(Dialog *d, int64 new_order, bool need_send_update_chat_order) {
  CHECK(d != nullptr);
  DialogDate old_date(d->order, d->dialog_id);
  DialogDate new_date(new_order, d->dialog_id);

  std::set<DialogDate> *ordered_dialogs_set = nullptr;
  switch (d->dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::Channel:
    case DialogType::SecretChat:
      ordered_dialogs_set = &ordered_server_dialogs_;
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }

  if (old_date == new_date) {
    if (new_order == DEFAULT_ORDER && ordered_dialogs_set->count(old_date) == 0) {
      ordered_dialogs_set->insert(new_date);
    }
    LOG(INFO) << "Dialog order is not changed: " << new_order;
    return false;
  }
  LOG(INFO) << "Update order of " << d->dialog_id << " from " << d->order << " to " << new_order;

  bool need_update = false;
  if (old_date <= last_dialog_date_) {
    if (ordered_dialogs_.erase(old_date) == 0) {
      UNREACHABLE();
    }
    need_update = true;
  }
  bool is_new = ordered_dialogs_set->erase(old_date) == 0;
  LOG_IF(ERROR, is_new && d->order != DEFAULT_ORDER) << d->dialog_id << " not found in the chat list";

  int64 updated_to = 0;
  if (new_date <= last_dialog_date_) {
    ordered_dialogs_.insert(new_date);
    need_update = true;
    updated_to = new_order;
  }
  ordered_dialogs_set->insert(new_date);

  bool add_to_hints = (d->order == DEFAULT_ORDER);

  if (!is_new && (d->order == DEFAULT_ORDER || new_order == DEFAULT_ORDER)) {
    auto unread_count = d->server_unread_count + d->local_unread_count;
    if (unread_count != 0 && is_unread_count_inited_) {
      const char *source = "on_dialog_join";
      if (d->order != DEFAULT_ORDER) {
        unread_count = -unread_count;
        source = "on_dialog_leave";
      } else {
        CHECK(new_order != DEFAULT_ORDER);
      }

      unread_message_total_count_ += unread_count;
      if (d->notification_settings.mute_until != 0) {
        unread_message_muted_count_ += unread_count;
      }
      send_update_unread_message_count(d->dialog_id, true, source);
    }
  }

  d->order = new_order;

  if (add_to_hints) {
    update_dialogs_hints(d);
  }
  update_dialogs_hints_rating(d);

  if (need_update && need_send_update_chat_order) {
    send_closure(G()->td(), &Td::send_update, make_tl_object<td_api::updateChatOrder>(d->dialog_id.get(), updated_to));
  }
  return true;
}

void MessagesManager::update_last_dialog_date() {
  auto old_last_dialog_date = last_dialog_date_;
  last_dialog_date_ = last_server_dialog_date_;  // std::min
  CHECK(old_last_dialog_date <= last_dialog_date_);

  LOG(INFO) << "Update last dialog date from " << old_last_dialog_date << " to " << last_dialog_date_;
  LOG(INFO) << "Know about " << ordered_server_dialogs_.size() << " chats";

  if (old_last_dialog_date != last_dialog_date_) {
    for (auto it = ordered_server_dialogs_.upper_bound(old_last_dialog_date);
         it != ordered_server_dialogs_.end() && *it <= last_dialog_date_; ++it) {
      auto dialog_id = it->get_dialog_id();
      auto d = get_dialog(dialog_id);
      CHECK(d != nullptr);
      ordered_dialogs_.insert(DialogDate(d->order, d->dialog_id));
      send_closure(G()->td(), &Td::send_update, make_tl_object<td_api::updateChatOrder>(d->dialog_id.get(), d->order));
    }

    if (last_dialog_date_ == MAX_DIALOG_DATE) {
      recalc_unread_message_count();
    }
  }

  if (G()->parameters().use_message_db && last_database_server_dialog_date_ < last_server_dialog_date_) {
    auto last_server_dialog_date_string = to_string(last_server_dialog_date_.get_order()) + " " +
                                          to_string(last_server_dialog_date_.get_dialog_id().get());
    G()->td_db()->get_binlog_pmc()->set("last_server_dialog_date", last_server_dialog_date_string);
    LOG(INFO) << "Save last server dialog date " << last_server_dialog_date_string;
    last_database_server_dialog_date_ = last_server_dialog_date_;
    last_loaded_database_dialog_date_ = last_server_dialog_date_;
  }
}

uint64 MessagesManager::get_sequence_dispatcher_id(DialogId dialog_id, int32 message_content_type) {
  switch (message_content_type) {
    case MessageAnimation::ID:
    case MessageAudio::ID:
    case MessageDocument::ID:
    case MessagePhoto::ID:
    case MessageSticker::ID:
    case MessageVideo::ID:
    case MessageVideoNote::ID:
    case MessageVoiceNote::ID:
      return static_cast<uint64>(dialog_id.get() * 2 + 1);
    default:
      return static_cast<uint64>(dialog_id.get() * 2 + 2);
  }
}

MessagesManager::Dialog *MessagesManager::get_dialog(DialogId dialog_id) {
  auto it = dialogs_.find(dialog_id);
  return it == dialogs_.end() ? nullptr : it->second.get();
}

const MessagesManager::Dialog *MessagesManager::get_dialog(DialogId dialog_id) const {
  auto it = dialogs_.find(dialog_id);
  return it == dialogs_.end() ? nullptr : it->second.get();
}

bool MessagesManager::have_dialog_force(DialogId dialog_id) {
  return loaded_dialogs_.count(dialog_id) > 0 || get_dialog_force(dialog_id) != nullptr;
}

MessagesManager::Dialog *MessagesManager::get_dialog_force(DialogId dialog_id) {
  // TODO remove most usages of that function, preload dialog asynchronously if possible
  auto it = dialogs_.find(dialog_id);
  if (it != dialogs_.end()) {
    return it->second.get();
  }

  if (!dialog_id.is_valid() || !G()->parameters().use_message_db || loaded_dialogs_.count(dialog_id) > 0) {
    return nullptr;
  }

  LOG(INFO) << "Try to load " << dialog_id << " from database";
  auto d = on_load_dialog_from_database(G()->td_db()->get_dialog_db_sync()->get_dialog(dialog_id));
  CHECK(d == nullptr || d->dialog_id == dialog_id);
  return d;
}

unique_ptr<MessagesManager::Dialog> MessagesManager::parse_dialog(DialogId dialog_id, const BufferSlice &value) {
  LOG(INFO) << "Loaded " << dialog_id << " from database";
  auto d = make_unique<Dialog>();
  std::fill(d->message_count_by_index.begin(), d->message_count_by_index.end(), -1);

  loaded_dialogs_.insert(dialog_id);

  log_event_parse(*d, value.as_slice()).ensure();
  CHECK(dialog_id == d->dialog_id);

  Dependencies dependencies;
  add_dialog_dependencies(dependencies, dialog_id);
  if (d->messages != nullptr) {
    add_message_dependencies(dependencies, dialog_id, d->messages.get());
  }
  resolve_dependencies_force(dependencies);

  return d;
}

MessagesManager::Dialog *MessagesManager::on_load_dialog_from_database(const Result<BufferSlice> &r_value) {
  CHECK(G()->parameters().use_message_db);

  if (!r_value.is_ok()) {
    return nullptr;
  }

  // hack
  LogEventParser dialog_id_parser(r_value.ok().as_slice());
  int32 flags;
  DialogId dialog_id;
  parse(flags, dialog_id_parser);
  parse(dialog_id, dialog_id_parser);

  auto old_d = get_dialog(dialog_id);
  if (old_d != nullptr) {
    return old_d;
  }

  return add_new_dialog(parse_dialog(dialog_id, r_value.ok()), true);
}

void MessagesManager::load_notification_settings() {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  if (!users_notification_settings_.is_synchronized) {
    td_->create_handler<GetNotifySettingsQuery>(Promise<>())->send(NOTIFICATION_SETTINGS_FOR_PRIVATE_CHATS);
  }
  if (!chats_notification_settings_.is_synchronized) {
    td_->create_handler<GetNotifySettingsQuery>(Promise<>())->send(NOTIFICATION_SETTINGS_FOR_GROUP_CHATS);
  }
  if (!dialogs_notification_settings_.is_synchronized) {
    td_->create_handler<GetNotifySettingsQuery>(Promise<>())->send(NOTIFICATION_SETTINGS_FOR_ALL_CHATS);
  }
}

string MessagesManager::get_channel_pts_key(DialogId dialog_id) {
  CHECK(dialog_id.get_type() == DialogType::Channel);
  auto channel_id = dialog_id.get_channel_id();
  return "ch.p" + to_string(channel_id.get());
}

int32 MessagesManager::load_channel_pts(DialogId dialog_id) const {
  auto pts = to_integer<int32>(G()->td_db()->get_binlog_pmc()->get(get_channel_pts_key(dialog_id)));
  LOG(INFO) << "Load " << dialog_id << " pts = " << pts;
  return pts;
}

void MessagesManager::set_channel_pts(Dialog *d, int32 new_pts, const char *source) const {
  CHECK(d != nullptr);
  CHECK(d->dialog_id.get_type() == DialogType::Channel);

  LOG_IF(ERROR, running_get_channel_difference(d->dialog_id))
      << "Set pts of " << d->dialog_id << " to " << new_pts << " while running getChannelDifference";

  if (is_debug_message_op_enabled()) {
    d->debug_message_op.emplace_back(Dialog::MessageOp::SetPts, MessageId(), new_pts, false, false, false, source);
  }

  // TODO delete_first_messages support in channels
  if (new_pts == std::numeric_limits<int32>::max()) {
    LOG(ERROR) << "Update " << d->dialog_id << " pts to -1";
    G()->td_db()->get_binlog_pmc()->erase(get_channel_pts_key(d->dialog_id));
    d->pts = std::numeric_limits<int32>::max();
    return;
  }
  if (new_pts > d->pts || (0 < new_pts && new_pts < d->pts - 99999)) {  // pts can only go up or drop cardinally
    if (new_pts < d->pts - 99999) {
      LOG(WARNING) << "Pts of " << d->dialog_id << " decreases from " << d->pts << " to " << new_pts;
    } else {
      LOG(INFO) << "Update " << d->dialog_id << " pts to " << new_pts;
    }

    d->pts = new_pts;
    G()->td_db()->get_binlog_pmc()->set(get_channel_pts_key(d->dialog_id), to_string(new_pts));
  } else if (new_pts < d->pts) {
    LOG(ERROR) << "Receive wrong pts " << new_pts << " in " << d->dialog_id << " . Current pts is " << d->pts;
  }
}

bool MessagesManager::running_get_channel_difference(DialogId dialog_id) const {
  return active_get_channel_differencies_.count(dialog_id) > 0;
}

class MessagesManager::GetChannelDifferenceLogEvent {
 public:
  ChannelId channel_id;
  int64 access_hash;

  GetChannelDifferenceLogEvent() : channel_id(), access_hash() {
  }

  GetChannelDifferenceLogEvent(ChannelId channel_id, int64 access_hash)
      : channel_id(channel_id), access_hash(access_hash) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(channel_id, storer);
    td::store(access_hash, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(channel_id, parser);
    td::parse(access_hash, parser);
  }
};

void MessagesManager::get_channel_difference(DialogId dialog_id, int32 pts, bool force, const char *source) {
  if (channel_get_difference_retry_timeout_.has_timeout(dialog_id.get())) {
    LOG(INFO) << "Skip running channels.getDifference for " << dialog_id << " from " << source
              << " because it is scheduled for later time";
    return;
  }

  auto input_channel = td_->contacts_manager_->get_input_channel(dialog_id.get_channel_id());
  if (input_channel == nullptr) {
    LOG(ERROR) << "Skip running channels.getDifference for " << dialog_id << " from " << source
               << " because have no info about the chat";
    after_get_channel_difference(dialog_id, false);
    return;
  }

  if (force && get_channel_difference_to_logevent_id_.count(dialog_id) == 0) {
    auto channel_id = dialog_id.get_channel_id();
    CHECK(input_channel->get_id() == telegram_api::inputChannel::ID);
    auto access_hash = static_cast<const telegram_api::inputChannel &>(*input_channel).access_hash_;
    auto logevent = GetChannelDifferenceLogEvent(channel_id, access_hash);
    auto storer = LogEventStorerImpl<GetChannelDifferenceLogEvent>(logevent);
    auto logevent_id =
        BinlogHelper::add(G()->td_db()->get_binlog(), LogEvent::HandlerType::GetChannelDifference, storer);

    get_channel_difference_to_logevent_id_.emplace(dialog_id, logevent_id);
  }

  return do_get_channel_difference(dialog_id, pts, force, std::move(input_channel), source);
}

void MessagesManager::do_get_channel_difference(DialogId dialog_id, int32 pts, bool force,
                                                tl_object_ptr<telegram_api::InputChannel> &&input_channel,
                                                const char *source) {
  auto inserted = active_get_channel_differencies_.emplace(dialog_id, source);
  if (!inserted.second) {
    LOG(INFO) << "Skip running channels.getDifference for " << dialog_id << " from " << source
              << " because it has already been run";
    return;
  }

  int32 limit = td_->auth_manager_->is_bot() ? MAX_BOT_CHANNEL_DIFFERENCE : MAX_CHANNEL_DIFFERENCE;
  if (pts <= 0) {
    pts = 1;
    limit = MIN_CHANNEL_DIFFERENCE;
  }

  LOG(INFO) << "-----BEGIN GET CHANNEL DIFFERENCE----- for " << dialog_id << " with pts " << pts << " and limit "
            << limit << " from " << source;

  td_->create_handler<GetChannelDifferenceQuery>()->send(dialog_id, std::move(input_channel), pts, limit, force);
}

void MessagesManager::process_get_channel_difference_updates(
    DialogId dialog_id, vector<tl_object_ptr<telegram_api::Message>> &&new_messages,
    vector<tl_object_ptr<telegram_api::Update>> &&other_updates) {
  LOG(INFO) << "In get channel difference for " << dialog_id << " receive " << new_messages.size() << " messages and "
            << other_updates.size() << " other updates";
  for (auto &update : other_updates) {
    if (update->get_id() == telegram_api::updateMessageID::ID) {
      auto sent_message_update = move_tl_object_as<telegram_api::updateMessageID>(update);
      on_update_message_id(sent_message_update->random_id_, MessageId(ServerMessageId(sent_message_update->id_)),
                           "get_channel_difference");
      update = nullptr;
    }
  }

  for (auto &message : new_messages) {
    on_get_message(std::move(message), true, true, true, true, "get channel difference");
  }

  for (auto &update : other_updates) {
    if (update != nullptr) {
      switch (update->get_id()) {
        case telegram_api::updateDeleteChannelMessages::ID:
          process_channel_update(std::move(update));
          break;
        case telegram_api::updateEditChannelMessage::ID:
          process_channel_update(std::move(update));
          break;
        default:
          LOG(ERROR) << "Unsupported update received in getChannelDifference: " << oneline(to_string(update));
          break;
      }
    }
  }
  CHECK(!running_get_channel_difference(dialog_id)) << '"' << active_get_channel_differencies_[dialog_id] << '"';
}

void MessagesManager::on_get_channel_dialog(DialogId dialog_id, MessageId last_message_id,
                                            MessageId read_inbox_max_message_id, int32 server_unread_count,
                                            int32 unread_mention_count, MessageId read_outbox_max_message_id,
                                            vector<tl_object_ptr<telegram_api::Message>> &&messages) {
  std::unordered_map<FullMessageId, tl_object_ptr<telegram_api::Message>, FullMessageIdHash> full_message_id_to_message;
  for (auto &message : messages) {
    auto message_id = get_message_id(message);
    auto message_dialog_id = get_message_dialog_id(message);
    if (!message_dialog_id.is_valid()) {
      message_dialog_id = dialog_id;
    }
    auto full_message_id = FullMessageId(message_dialog_id, message_id);
    full_message_id_to_message[full_message_id] = std::move(message);
  }

  FullMessageId last_full_message_id(dialog_id, last_message_id);
  if (last_message_id.is_valid()) {
    if (full_message_id_to_message.count(last_full_message_id) == 0) {
      LOG(ERROR) << "Last " << last_message_id << " in " << dialog_id << " not found. Have:";
      for (auto &message : full_message_id_to_message) {
        LOG(ERROR) << to_string(message.second);
      }
      return;
    }
  } else {
    LOG(ERROR) << "Receive as last " << last_message_id;
    return;
  }

  Dialog *d = get_dialog(dialog_id);
  CHECK(d != nullptr);

  // TODO gaps support
  // There are many ways of handling a gap in a channel:
  // 1) Delete all known messages in the chat, begin from scratch. It is easy to implement, but suddenly disappearing
  //    messages looks awful for the user.
  // 2) Save all messages loaded in the memory until application restart, but delete all messages from database.
  //    Messages left in the memory must be lazily updated using calls to getHistory. It looks much smoothly for the
  //    user, he will need to redownload messages only after client restart. Unsynchronized messages left in the
  //    memory shouldn't be saved to database, results of getHistory and getMessage must be used to update state of
  //    deleted and edited messages left in the memory.
  // 3) Save all messages loaded in the memory and stored in the database without saving that some messages form
  //    continuous ranges. Messages in the database will be excluded from results of getChatHistory and
  //    searchChatMessages after application restart and will be available only through getMessage.
  //    Every message should still be checked using getHistory. It has more disadvantages over 2) than advantages.
  // 4) Save all messages with saving all data about continuous message ranges. Messages from the database may be used
  //    as results of getChatHistory and (if implemented continuous ranges support for searching shared media)
  //    searchChatMessages. The messages should still be lazily checked using getHistory, but they are still available
  //    offline. It is the best way for gaps support, but it is pretty hard to implement correctly.
  // It should be also noted that some messages like live location messages shouldn't be deleted.
  // delete_all_dialog_messages_from_database(dialog_id, d->last_database_message_id, "on_get_channel_dialog");

  set_dialog_first_database_message_id(d, MessageId(), "on_get_channel_dialog");
  set_dialog_last_database_message_id(d, MessageId(), "on_get_channel_dialog");
  d->have_full_history = false;
  for (auto &first_message_id : d->first_database_message_id_by_index) {
    first_message_id = MessageId();
  }
  std::fill(d->message_count_by_index.begin(), d->message_count_by_index.end(), -1);

  on_dialog_updated(dialog_id, "on_get_channel_dialog 10");

  // TODO support last_message_id.get() < d->last_new_message_id.get()
  if (last_message_id.get() > d->last_new_message_id.get()) {  // if last message is really a new message
    d->last_new_message_id = MessageId();
    set_dialog_last_message_id(d, MessageId(), "on_get_channel_dialog 20");
    send_update_chat_last_message(d, "on_get_channel_dialog 30");
    auto added_full_message_id = on_get_message(std::move(full_message_id_to_message[last_full_message_id]), true, true,
                                                true, true, "channel difference too long");
    if (added_full_message_id.get_message_id().is_valid()) {
      if (added_full_message_id.get_message_id() == d->last_new_message_id) {
        CHECK(last_full_message_id == added_full_message_id);
        CHECK(d->last_message_id == d->last_new_message_id);
      } else {
        LOG(ERROR) << added_full_message_id << " doesn't became last new message";
        dump_debug_message_op(d, 2);
      }
    } else {
      set_dialog_last_new_message_id(d, last_full_message_id.get_message_id(),
                                     "on_get_channel_dialog 40");  // skip updates about some messages
    }
  }

  if (d->server_unread_count != server_unread_count || d->last_read_inbox_message_id != read_inbox_max_message_id) {
    set_dialog_last_read_inbox_message_id(d, read_inbox_max_message_id, server_unread_count, d->local_unread_count,
                                          false, "on_get_channel_dialog 50");
  }
  if (d->unread_mention_count != unread_mention_count) {
    d->unread_mention_count = unread_mention_count;
    d->message_count_by_index[search_messages_filter_index(SearchMessagesFilter::UnreadMention)] =
        d->unread_mention_count;
    send_update_chat_unread_mention_count(d);
  }

  if (d->last_read_outbox_message_id != read_outbox_max_message_id) {
    set_dialog_last_read_outbox_message_id(d, read_outbox_max_message_id);
  }
}

void MessagesManager::on_get_channel_difference(
    DialogId dialog_id, int32 request_pts, int32 request_limit,
    tl_object_ptr<telegram_api::updates_ChannelDifference> &&difference_ptr) {
  LOG(INFO) << "----- END  GET CHANNEL DIFFERENCE----- for " << dialog_id;
  CHECK(active_get_channel_differencies_.count(dialog_id) == 1);
  active_get_channel_differencies_.erase(dialog_id);
  auto d = get_dialog_force(dialog_id);

  if (difference_ptr == nullptr) {
    bool have_access = have_input_peer(dialog_id, AccessRights::Read);
    if (have_access) {
      CHECK(d != nullptr);
      channel_get_difference_retry_timeout_.add_timeout_in(dialog_id.get(), d->retry_get_difference_timeout);
      d->retry_get_difference_timeout *= 2;
      if (d->retry_get_difference_timeout > 60) {
        d->retry_get_difference_timeout = Random::fast(60, 80);
      }
    } else {
      after_get_channel_difference(dialog_id, false);
    }
    return;
  }

  bool need_update_dialog_pos = false;
  if (d == nullptr) {
    d = add_dialog(dialog_id);
    need_update_dialog_pos = true;
  }

  int32 cur_pts = d->pts <= 0 ? 1 : d->pts;
  LOG_IF(ERROR, cur_pts != request_pts) << "Channel pts has changed from " << request_pts << " to " << d->pts << " in "
                                        << dialog_id << " during getChannelDifference";

  LOG(INFO) << "Receive result of getChannelDifference for " << dialog_id << " with pts = " << request_pts
            << " and limit = " << request_limit << ": " << to_string(difference_ptr);

  d->retry_get_difference_timeout = 1;

  bool is_final = true;
  int32 timeout = 0;
  switch (difference_ptr->get_id()) {
    case telegram_api::updates_channelDifferenceEmpty::ID: {
      auto difference = move_tl_object_as<telegram_api::updates_channelDifferenceEmpty>(difference_ptr);
      int32 flags = difference->flags_;
      is_final = (flags & CHANNEL_DIFFERENCE_FLAG_IS_FINAL) != 0;
      LOG_IF(ERROR, !is_final) << "Receive channelDifferenceEmpty as result of getChannelDifference with pts = "
                               << request_pts << " and limit = " << request_limit << " in " << dialog_id
                               << ", but it is not final";
      if (flags & CHANNEL_DIFFERENCE_FLAG_HAS_TIMEOUT) {
        timeout = difference->timeout_;
      }

      // bots can receive channelDifferenceEmpty with pts bigger than known pts
      LOG_IF(ERROR, request_pts != difference->pts_ && !td_->auth_manager_->is_bot())
          << "Receive channelDifferenceEmpty as result of getChannelDifference with pts = " << request_pts
          << " and limit = " << request_limit << " in " << dialog_id << ", but pts has changed from " << request_pts
          << " to " << difference->pts_;
      set_channel_pts(d, difference->pts_, "channel difference empty");
      break;
    }
    case telegram_api::updates_channelDifference::ID: {
      auto difference = move_tl_object_as<telegram_api::updates_channelDifference>(difference_ptr);

      int32 flags = difference->flags_;
      is_final = (flags & CHANNEL_DIFFERENCE_FLAG_IS_FINAL) != 0;
      if (flags & CHANNEL_DIFFERENCE_FLAG_HAS_TIMEOUT) {
        timeout = difference->timeout_;
      }

      auto new_pts = difference->pts_;
      if (request_pts >= new_pts) {
        LOG(ERROR) << "Receive channelDifference as result of getChannelDifference with pts = " << request_pts
                   << " and limit = " << request_limit << " in " << dialog_id << ", but pts has changed from " << d->pts
                   << " to " << new_pts << ". Difference: " << oneline(to_string(difference));
        new_pts = request_pts + 1;
      }

      td_->contacts_manager_->on_get_users(std::move(difference->users_));
      td_->contacts_manager_->on_get_chats(std::move(difference->chats_));

      process_get_channel_difference_updates(dialog_id, std::move(difference->new_messages_),
                                             std::move(difference->other_updates_));

      set_channel_pts(d, new_pts, "channel difference");
      break;
    }
    case telegram_api::updates_channelDifferenceTooLong::ID: {
      auto difference = move_tl_object_as<telegram_api::updates_channelDifferenceTooLong>(difference_ptr);

      int32 flags = difference->flags_;
      is_final = (flags & CHANNEL_DIFFERENCE_FLAG_IS_FINAL) != 0;
      if (flags & CHANNEL_DIFFERENCE_FLAG_HAS_TIMEOUT) {
        timeout = difference->timeout_;
      }

      auto new_pts = difference->pts_;
      if (request_pts + request_limit > new_pts) {
        LOG(ERROR) << "Receive channelDifferenceTooLong as result of getChannelDifference with pts = " << request_pts
                   << " and limit = " << request_limit << " in " << dialog_id << ", but pts has changed from " << d->pts
                   << " to " << new_pts << ". Difference: " << oneline(to_string(difference));
        if (request_pts >= new_pts) {
          new_pts = request_pts + 1;
        }
      }

      td_->contacts_manager_->on_get_users(std::move(difference->users_));
      td_->contacts_manager_->on_get_chats(std::move(difference->chats_));

      on_get_channel_dialog(dialog_id, MessageId(ServerMessageId(difference->top_message_)),
                            MessageId(ServerMessageId(difference->read_inbox_max_id_)), difference->unread_count_,
                            difference->unread_mentions_count_,
                            MessageId(ServerMessageId(difference->read_outbox_max_id_)),
                            std::move(difference->messages_));
      need_update_dialog_pos = true;

      set_channel_pts(d, new_pts, "channel difference too long");
      break;
    }
    default:
      UNREACHABLE();
  }

  if (need_update_dialog_pos) {
    update_dialog_pos(d, false, "on_get_channel_difference");
  }

  if (!is_final) {
    LOG_IF(ERROR, timeout > 0) << "Have timeout in not final ChannelDifference in " << dialog_id;
    get_channel_difference(dialog_id, d->pts, true, "on_get_channel_difference");
    return;
  }

  LOG_IF(ERROR, timeout == 0) << "Have no timeout in final ChannelDifference in " << dialog_id;
  if (timeout > 0 && d->is_opened) {
    channel_get_difference_timeout_.add_timeout_in(dialog_id.get(), timeout);
  }
  after_get_channel_difference(dialog_id, true);
}

void MessagesManager::after_get_channel_difference(DialogId dialog_id, bool success) {
  CHECK(!running_get_channel_difference(dialog_id)) << '"' << active_get_channel_differencies_[dialog_id] << '"';

  auto logevent_it = get_channel_difference_to_logevent_id_.find(dialog_id);
  if (logevent_it != get_channel_difference_to_logevent_id_.end()) {
    BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_it->second);
    get_channel_difference_to_logevent_id_.erase(logevent_it);
  }

  auto d = get_dialog(dialog_id);
  bool have_access = have_input_peer(dialog_id, AccessRights::Read);
  if (!have_access) {
    if (d != nullptr) {
      d->postponed_channel_updates.clear();
    }
  } else if (d->postponed_channel_updates.size()) {
    LOG(INFO) << "Begin to apply postponed channel updates";
    while (!d->postponed_channel_updates.empty()) {
      auto it = d->postponed_channel_updates.begin();
      auto update = std::move(it->second.update);
      auto update_pts = it->second.pts;
      auto update_pts_count = it->second.pts_count;
      d->postponed_channel_updates.erase(it);
      auto old_size = d->postponed_channel_updates.size();
      auto update_id = update->get_id();
      add_pending_channel_update(dialog_id, std::move(update), update_pts, update_pts_count,
                                 "apply postponed channel updates", true);
      if (d->postponed_channel_updates.size() != old_size || running_get_channel_difference(dialog_id)) {
        if (success && update_pts < d->pts + 10000 && update_pts_count == 1) {
          // if getChannelDifference was successful and update pts is near channel pts,
          // we hope that the update eventually can be applied
          LOG(INFO) << "Can't apply postponed channel updates";
        } else {
          // otherwise we protecting from getChannelDifference repeating calls by dropping pending updates
          LOG(ERROR) << "Failed to apply postponed updates of type " << update_id << " in " << dialog_id << " with pts "
                     << d->pts << ", update pts is " << update_pts << ", update pts count is " << update_pts_count;
          d->postponed_channel_updates.clear();
        }
        break;
      }
    }
    LOG(INFO) << "Finish to apply postponed channel updates";
  }

  if (postponed_chat_read_inbox_updates_.erase(dialog_id) > 0) {
    send_update_chat_read_inbox(d, true, "after_get_channel_difference");
  }

  auto it_get_message_requests = postponed_get_message_requests_.find(dialog_id);
  if (it_get_message_requests != postponed_get_message_requests_.end()) {
    CHECK(d != nullptr);
    for (auto &request : it_get_message_requests->second) {
      auto message_id = request.first;
      LOG(INFO) << "Run postponed getMessage request for " << message_id << " in " << dialog_id;
      if (d->last_new_message_id != MessageId() && message_id.get() > d->last_new_message_id.get()) {
        // message will not be added to the dialog anyway, get channel difference didn't help
        request.second.set_value(Unit());
      } else {
        get_messages_from_server({FullMessageId{dialog_id, message_id}}, std::move(request.second));
      }
    }
    postponed_get_message_requests_.erase(it_get_message_requests);
  }

  auto it = pending_channel_on_get_dialogs_.find(dialog_id);
  if (it != pending_channel_on_get_dialogs_.end()) {
    LOG(INFO) << "Apply postponed results of channel getDialogs for " << dialog_id;
    PendingOnGetDialogs res = std::move(it->second);
    pending_channel_on_get_dialogs_.erase(it);

    on_get_dialogs(std::move(res.dialogs), res.total_count, std::move(res.messages), std::move(res.promise));
  }

  // TODO resend some messages
}

void MessagesManager::update_used_hashtags(DialogId dialog_id, const Message *m) {
  CHECK(m != nullptr);
  if (m->via_bot_user_id.is_valid() || m->content->get_id() != MessageText::ID) {
    return;
  }
  auto message_text = static_cast<const MessageText *>(m->content.get());
  const unsigned char *ptr = Slice(message_text->text.text).ubegin();
  const unsigned char *end = Slice(message_text->text.text).uend();
  int32 utf16_pos = 0;
  for (auto &entity : message_text->text.entities) {
    if (entity.type != MessageEntity::Type::Hashtag) {
      continue;
    }
    while (utf16_pos < entity.offset && ptr < end) {
      utf16_pos += 1 + (ptr[0] >= 0xf0);
      ptr = next_utf8_unsafe(ptr, nullptr);
    }
    CHECK(utf16_pos == entity.offset);
    auto from = ptr;

    while (utf16_pos < entity.offset + entity.length && ptr < end) {
      utf16_pos += 1 + (ptr[0] >= 0xf0);
      ptr = next_utf8_unsafe(ptr, nullptr);
    }
    CHECK(utf16_pos == entity.offset + entity.length);
    auto to = ptr;

    send_closure(td_->hashtag_hints_, &HashtagHints::hashtag_used, Slice(from + 1, to).str());
  }
}

MessagesManager::Message *MessagesManager::continue_send_message(DialogId dialog_id, unique_ptr<Message> &&m,
                                                                 uint64 logevent_id) {
  Dialog *d = get_dialog_force(dialog_id);
  if (d == nullptr) {
    LOG(ERROR) << "Can't find " << dialog_id << " to resend a message";
    BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
    return nullptr;
  }

  m->message_id = get_next_yet_unsent_message_id(d);
  m->random_y = get_random_y(m->message_id);
  m->date = G()->unix_time();
  m->have_previous = true;
  m->have_next = true;

  LOG(INFO) << "Continue to send " << m->message_id << " to " << dialog_id << " from binlog";

  if (!have_input_peer(dialog_id, AccessRights::Read)) {
    BinlogHelper::erase(G()->td_db()->get_binlog(), logevent_id);
    return nullptr;
  }

  message_random_ids_.insert(m->random_id);

  bool need_update = false;
  bool need_update_dialog_pos = false;
  auto result_message =
      add_message_to_dialog(d, std::move(m), false, &need_update, &need_update_dialog_pos, "resend message");
  CHECK(result_message != nullptr);
  // CHECK(need_update_dialog_pos == true);

  auto can_send_status = can_send_message(dialog_id);
  if (can_send_status.is_error()) {
    LOG(INFO) << "Can't resend a message to " << dialog_id << ": " << can_send_status.error();

    int64 random_id = begin_send_message(dialog_id, result_message);
    on_send_message_fail(random_id, can_send_status.move_as_error());
    return nullptr;
  }

  send_update_new_message(d, result_message);
  if (need_update_dialog_pos) {
    send_update_chat_last_message(d, "on_resend_message");
  }
  return result_message;
}

void MessagesManager::on_binlog_events(vector<BinlogEvent> &&events) {
  for (auto &event : events) {
    switch (event.type_) {
      case LogEvent::HandlerType::SendMessage: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        SendMessageLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id;
        auto m = std::move(log_event.m_out);
        m->send_message_logevent_id = event.id_;

        if (m->content->get_id() == MessageUnsupported::ID) {
          LOG(ERROR) << "Message content is invalid: " << format::as_hex_dump<4>(Slice(event.data_));
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          continue;
        }

        Dependencies dependencies;
        add_dialog_dependencies(dependencies, dialog_id);
        add_message_dependencies(dependencies, dialog_id, m.get());
        resolve_dependencies_force(dependencies);

        m->content = dup_message_content(dialog_id, m->content.get(), false);

        auto result_message = continue_send_message(dialog_id, std::move(m), event.id_);
        if (result_message != nullptr) {
          do_send_message(dialog_id, result_message);
        }
        break;
      }
      case LogEvent::HandlerType::SendBotStartMessage: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        SendBotStartMessageLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id;
        auto m = std::move(log_event.m_out);
        m->send_message_logevent_id = event.id_;

        CHECK(m->content->get_id() == MessageText::ID);

        Dependencies dependencies;
        add_dialog_dependencies(dependencies, dialog_id);
        add_message_dependencies(dependencies, dialog_id, m.get());
        resolve_dependencies_force(dependencies);

        auto bot_user_id = log_event.bot_user_id;
        if (!td_->contacts_manager_->have_user_force(bot_user_id)) {
          LOG(ERROR) << "Can't find bot " << bot_user_id;
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          continue;
        }

        auto result_message = continue_send_message(dialog_id, std::move(m), event.id_);
        if (result_message != nullptr) {
          do_send_bot_start_message(bot_user_id, dialog_id, log_event.parameter, result_message);
        }
        break;
      }
      case LogEvent::HandlerType::SendInlineQueryResultMessage: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        SendInlineQueryResultMessageLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id;
        auto m = std::move(log_event.m_out);
        m->send_message_logevent_id = event.id_;

        if (m->content->get_id() == MessageUnsupported::ID) {
          LOG(ERROR) << "Message content is invalid: " << format::as_hex_dump<4>(Slice(event.data_));
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          continue;
        }

        Dependencies dependencies;
        add_dialog_dependencies(dependencies, dialog_id);
        add_message_dependencies(dependencies, dialog_id, m.get());
        resolve_dependencies_force(dependencies);

        m->content = dup_message_content(dialog_id, m->content.get(), false);

        auto result_message = continue_send_message(dialog_id, std::move(m), event.id_);
        if (result_message != nullptr) {
          do_send_inline_query_result_message(dialog_id, result_message, log_event.query_id, log_event.result_id);
        }
        break;
      }
      case LogEvent::HandlerType::SendScreenshotTakenNotificationMessage: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        SendScreenshotTakenNotificationMessageLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id;
        auto m = std::move(log_event.m_out);
        m->send_message_logevent_id = 0;

        CHECK(m->content->get_id() == MessageScreenshotTaken::ID);

        Dependencies dependencies;
        add_dialog_dependencies(dependencies, dialog_id);
        add_message_dependencies(dependencies, dialog_id, m.get());
        resolve_dependencies_force(dependencies);

        auto result_message = continue_send_message(dialog_id, std::move(m), event.id_);
        if (result_message != nullptr) {
          do_send_screenshot_taken_notification_message(dialog_id, result_message, event.id_);
        }
        break;
      }
      case LogEvent::HandlerType::ForwardMessages: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        ForwardMessagesLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto to_dialog_id = log_event.to_dialog_id;
        auto from_dialog_id = log_event.from_dialog_id;
        auto messages = std::move(log_event.messages_out);

        Dependencies dependencies;
        add_dialog_dependencies(dependencies, to_dialog_id);
        add_dialog_dependencies(dependencies, from_dialog_id);
        for (auto &m : messages) {
          add_message_dependencies(dependencies, to_dialog_id, m.get());
        }
        resolve_dependencies_force(dependencies);

        Dialog *to_dialog = get_dialog_force(to_dialog_id);
        if (to_dialog == nullptr) {
          LOG(ERROR) << "Can't find " << to_dialog_id << " to forward messages to";
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          continue;
        }
        Dialog *from_dialog = get_dialog_force(from_dialog_id);
        if (from_dialog == nullptr) {
          LOG(ERROR) << "Can't find " << from_dialog_id << " to forward messages from";
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          continue;
        }
        for (auto &m : messages) {
          m->message_id = get_next_yet_unsent_message_id(to_dialog);
          m->random_y = get_random_y(m->message_id);
          m->date = G()->unix_time();
          m->content = dup_message_content(to_dialog_id, m->content.get(), true);
          m->have_previous = true;
          m->have_next = true;
        }

        LOG(INFO) << "Continue to forward " << messages.size() << " messages to " << to_dialog_id << " from binlog";

        if (!have_input_peer(from_dialog_id, AccessRights::Read) || can_send_message(to_dialog_id).is_error()) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        bool need_update = false;
        bool need_update_dialog_pos = false;
        vector<Message *> forwarded_messages;
        for (auto &m : messages) {
          message_random_ids_.insert(m->random_id);
          forwarded_messages.push_back(add_message_to_dialog(to_dialog, std::move(m), false, &need_update,
                                                             &need_update_dialog_pos, "forward message again"));
          send_update_new_message(to_dialog, forwarded_messages.back());
        }
        if (need_update_dialog_pos) {
          send_update_chat_last_message(to_dialog, "on_reforward_message");
        }

        do_forward_messages(to_dialog_id, from_dialog_id, forwarded_messages, log_event.message_ids, event.id_);
        break;
      }
      case LogEvent::HandlerType::DeleteMessage: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        DeleteMessageLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();
        log_event.id_ = event.id_;
        do_delete_message_logevent(log_event);
        break;
      }
      case LogEvent::HandlerType::DeleteMessagesFromServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        DeleteMessagesFromServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id_;
        Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !have_input_peer(dialog_id, AccessRights::Read)) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        delete_messages_from_server(dialog_id, std::move(log_event.message_ids_), log_event.revoke_, event.id_, Auto());
        break;
      }
      case LogEvent::HandlerType::DeleteDialogHistoryFromServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        DeleteDialogHistoryFromServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id_;
        Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !have_input_peer(dialog_id, AccessRights::Read)) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        delete_dialog_history_from_server(dialog_id, log_event.max_message_id_, log_event.remove_from_dialog_list_,
                                          true, event.id_, Auto());
        break;
      }
      case LogEvent::HandlerType::DeleteAllChannelMessagesFromUserOnServer: {
        if (!G()->parameters().use_chat_info_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        DeleteAllChannelMessagesFromUserOnServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto channel_id = log_event.channel_id_;
        if (!td_->contacts_manager_->have_channel_force(channel_id)) {
          LOG(ERROR) << "Can't find " << channel_id;
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        auto user_id = log_event.user_id_;
        if (!td_->contacts_manager_->have_user_force(user_id)) {
          LOG(ERROR) << "Can't find user " << user_id;
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        delete_all_channel_messages_from_user_on_server(channel_id, user_id, event.id_, Auto());
        break;
      }
      case LogEvent::HandlerType::ReadHistoryOnServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        ReadHistoryOnServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id_;
        Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !have_input_peer(dialog_id, AccessRights::Read)) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        read_history_on_server(dialog_id, log_event.max_message_id_, true, event.id_);
        break;
      }
      case LogEvent::HandlerType::ReadMessageContentsOnServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        ReadMessageContentsOnServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id_;
        Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !have_input_peer(dialog_id, AccessRights::Read)) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        read_message_contents_on_server(dialog_id, std::move(log_event.message_ids_), event.id_);
        break;
      }
      case LogEvent::HandlerType::ReadAllDialogMentionsOnServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        ReadAllDialogMentionsOnServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id_;
        Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !have_input_peer(dialog_id, AccessRights::Read)) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        read_all_dialog_mentions_on_server(dialog_id, event.id_, Promise<Unit>());
        break;
      }
      case LogEvent::HandlerType::ToggleDialogIsPinnedOnServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        ToggleDialogIsPinnedOnServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id_;
        Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !have_input_peer(dialog_id, AccessRights::Read)) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        toggle_dialog_is_pinned_on_server(dialog_id, log_event.is_pinned_, event.id_);
        break;
      }
      case LogEvent::HandlerType::ReorderPinnedDialogsOnServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        ReorderPinnedDialogsOnServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        vector<DialogId> dialog_ids;
        for (auto &dialog_id : log_event.dialog_ids_) {
          Dialog *d = get_dialog_force(dialog_id);
          if (d != nullptr && have_input_peer(dialog_id, AccessRights::Read)) {
            dialog_ids.push_back(dialog_id);
          }
        }
        if (dialog_ids.empty()) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        reorder_pinned_dialogs_on_server(dialog_ids, event.id_);
        break;
      }
      case LogEvent::HandlerType::SaveDialogDraftMessageOnServer: {
        if (!G()->parameters().use_message_db) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        SaveDialogDraftMessageOnServerLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        auto dialog_id = log_event.dialog_id_;
        Dialog *d = get_dialog_force(dialog_id);
        if (d == nullptr || !have_input_peer(dialog_id, AccessRights::Write)) {
          BinlogHelper::erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }
        d->save_draft_message_logevent_id = event.id_;

        save_dialog_draft_message_on_server(dialog_id);
        break;
      }
      case LogEvent::HandlerType::GetChannelDifference: {
        GetChannelDifferenceLogEvent log_event;
        log_event_parse(log_event, event.data_).ensure();

        DialogId dialog_id(log_event.channel_id);
        LOG(INFO) << "Continue to run getChannelDifference in " << dialog_id;
        get_channel_difference_to_logevent_id_.emplace(dialog_id, event.id_);
        do_get_channel_difference(
            dialog_id, load_channel_pts(dialog_id), true,
            telegram_api::make_object<telegram_api::inputChannel>(log_event.channel_id.get(), log_event.access_hash),
            "LogEvent::HandlerType::GetChannelDifference");
        break;
      }
      default:
        LOG(FATAL) << "Unsupported logevent type " << event.type_;
    }
  }
}

void MessagesManager::save_recently_found_dialogs() {
  if (recently_found_dialogs_loaded_ < 2) {
    return;
  }

  string value;
  for (auto &dialog_id : recently_found_dialog_ids_) {
    if (!value.empty()) {
      value += ',';
    }
    if (!G()->parameters().use_message_db) {
      // if there is no dialog database, prefer to save dialogs by username
      auto username = get_dialog_username(dialog_id);
      if (dialog_id.get_type() != DialogType::SecretChat && !username.empty()) {
        value += '@';
        value += username;
        continue;
      }
    }
    value += to_string(dialog_id.get());
  }
  G()->td_db()->get_binlog_pmc()->set("recently_found_dialog_usernames_and_ids", value);
}

bool MessagesManager::load_recently_found_dialogs(Promise<Unit> &promise) {
  if (recently_found_dialogs_loaded_ >= 2) {
    return true;
  }

  string found_dialogs_str = G()->td_db()->get_binlog_pmc()->get("recently_found_dialog_usernames_and_ids");
  auto found_dialogs = full_split(found_dialogs_str, ',');
  if (found_dialogs.empty()) {
    recently_found_dialogs_loaded_ = 2;
    if (!recently_found_dialog_ids_.empty()) {
      save_recently_found_dialogs();
    }
    return true;
  }

  if (recently_found_dialogs_loaded_ == 1 && resolve_recent_found_dialogs_multipromise_.promise_count() == 0) {
    // queries was sent and have already been finished
    auto newly_found_dialogs = std::move(recently_found_dialog_ids_);
    recently_found_dialog_ids_.clear();

    for (auto it = found_dialogs.rbegin(); it != found_dialogs.rend(); ++it) {
      if ((*it)[0] == '@') {
        auto dialog_id = resolve_dialog_username(it->substr(1));
        if (dialog_id.is_valid() && have_input_peer(dialog_id, AccessRights::Read)) {
          force_create_dialog(dialog_id, "recently found resolved dialog");
          add_recently_found_dialog_internal(dialog_id);
        }
      } else {
        auto dialog_id = DialogId(to_integer<int64>(*it));
        CHECK(dialog_id.is_valid());
        if (have_input_peer(dialog_id, AccessRights::Read)) {
          force_create_dialog(dialog_id, "recently found dialog");
          add_recently_found_dialog_internal(dialog_id);
        }
      }
    }
    for (auto it = newly_found_dialogs.rbegin(); it != newly_found_dialogs.rend(); ++it) {
      add_recently_found_dialog_internal(*it);
    }
    recently_found_dialogs_loaded_ = 2;
    if (!newly_found_dialogs.empty()) {
      save_recently_found_dialogs();
    }
    return true;
  }

  resolve_recent_found_dialogs_multipromise_.add_promise(std::move(promise));
  if (recently_found_dialogs_loaded_ == 0) {
    recently_found_dialogs_loaded_ = 1;

    resolve_recent_found_dialogs_multipromise_.set_ignore_errors(true);

    for (auto &found_dialog : found_dialogs) {
      if (found_dialog[0] == '@') {
        search_public_dialog(found_dialog, false, resolve_recent_found_dialogs_multipromise_.get_promise());
      }
    }
    if (G()->parameters().use_message_db) {
      for (auto &found_dialog : found_dialogs) {
        if (found_dialog[0] != '@') {
          auto dialog_id = DialogId(to_integer<int64>(found_dialog));
          CHECK(dialog_id.is_valid());
          // TODO use asynchronous load
          // get_dialog(dialog_id, resolve_recent_found_dialogs_multipromise_.get_promise());
          get_dialog_force(dialog_id);
        }
      }
      resolve_recent_found_dialogs_multipromise_.get_promise().set_value(Unit());
    } else {
      get_dialogs(MIN_DIALOG_DATE, MAX_GET_DIALOGS, false, resolve_recent_found_dialogs_multipromise_.get_promise());
      td_->contacts_manager_->search_contacts("", 1, resolve_recent_found_dialogs_multipromise_.get_promise());
    }
  }
  return false;
}

Status MessagesManager::add_recently_found_dialog(DialogId dialog_id) {
  if (!have_dialog_force(dialog_id)) {
    return Status::Error(5, "Chat not found");
  }
  if (add_recently_found_dialog_internal(dialog_id)) {
    save_recently_found_dialogs();
  }

  return Status::OK();
}

Status MessagesManager::remove_recently_found_dialog(DialogId dialog_id) {
  if (!have_dialog_force(dialog_id)) {
    return Status::Error(5, "Chat not found");
  }
  if (remove_recently_found_dialog_internal(dialog_id)) {
    save_recently_found_dialogs();
  }

  return Status::OK();
}

void MessagesManager::clear_recently_found_dialogs() {
  recently_found_dialogs_loaded_ = 2;
  if (recently_found_dialog_ids_.empty()) {
    return;
  }

  recently_found_dialog_ids_.clear();
  save_recently_found_dialogs();
}

bool MessagesManager::add_recently_found_dialog_internal(DialogId dialog_id) {
  CHECK(have_dialog(dialog_id));

  if (!recently_found_dialog_ids_.empty() && recently_found_dialog_ids_[0] == dialog_id) {
    return false;
  }

  // TODO create function
  auto it = std::find(recently_found_dialog_ids_.begin(), recently_found_dialog_ids_.end(), dialog_id);
  if (it == recently_found_dialog_ids_.end()) {
    if (narrow_cast<int32>(recently_found_dialog_ids_.size()) == MAX_RECENT_FOUND_DIALOGS) {
      CHECK(!recently_found_dialog_ids_.empty());
      recently_found_dialog_ids_.back() = dialog_id;
    } else {
      recently_found_dialog_ids_.push_back(dialog_id);
    }
    it = recently_found_dialog_ids_.end() - 1;
  }
  std::rotate(recently_found_dialog_ids_.begin(), it, it + 1);
  return true;
}

bool MessagesManager::remove_recently_found_dialog_internal(DialogId dialog_id) {
  CHECK(have_dialog(dialog_id));

  auto it = std::find(recently_found_dialog_ids_.begin(), recently_found_dialog_ids_.end(), dialog_id);
  if (it == recently_found_dialog_ids_.end()) {
    return false;
  }
  recently_found_dialog_ids_.erase(it);
  return true;
}

void MessagesManager::suffix_load_loop(Dialog *d) {
  if (d->suffix_load_has_query_) {
    return;
  }

  if (d->suffix_load_queries_.empty()) {
    return;
  }
  CHECK(!d->suffix_load_done_);

  // Send db query
  LOG(INFO) << "suffix_load send query " << d->dialog_id;
  auto promise = PromiseCreator::lambda([actor_id = actor_id(this), dialog_id = d->dialog_id](Result<Unit> result) {
    send_closure(actor_id, &MessagesManager::suffix_load_query_ready, dialog_id);
  });
  d->suffix_load_query_message_id_ = d->suffix_load_first_message_id_;
  if (d->suffix_load_first_message_id_.is_valid()) {
    get_history(d->dialog_id, d->suffix_load_first_message_id_, -1, 100, true, true, std::move(promise));
  } else {
    get_history_from_the_end(d->dialog_id, true, true, std::move(promise));
  }
  d->suffix_load_has_query_ = true;
}

void MessagesManager::suffix_load_update_first_message_id(Dialog *d) {
  // Update first_message_id
  if (!d->suffix_load_first_message_id_.is_valid()) {
    d->suffix_load_first_message_id_ = d->last_message_id;
  }
  if (!d->suffix_load_first_message_id_.is_valid() && !d->suffix_load_was_query_) {
    return;
  }
  auto it = d->suffix_load_first_message_id_.is_valid() ? MessagesConstIterator(d, d->suffix_load_first_message_id_)
                                                        : MessagesConstIterator(d, MessageId::max());
  while (*it && (*it)->have_previous) {
    --it;
  }
  if (*it) {
    d->suffix_load_first_message_id_ = (*it)->message_id;
  } else {
    d->suffix_load_first_message_id_ = MessageId();
  }
}

void MessagesManager::suffix_load_query_ready(DialogId dialog_id) {
  LOG(INFO) << "suffix_load_query_ready " << dialog_id;
  auto *d = get_dialog(dialog_id);
  CHECK(d != nullptr);
  d->suffix_load_was_query_ = true;
  suffix_load_update_first_message_id(d);
  if (d->suffix_load_first_message_id_ == d->suffix_load_query_message_id_) {
    LOG(INFO) << "suffix_load done " << dialog_id;
    d->suffix_load_done_ = true;
  }
  d->suffix_load_has_query_ = false;

  // Remove ready queries
  auto *m = get_message_force(d, d->suffix_load_first_message_id_);
  auto ready_it = std::partition(d->suffix_load_queries_.begin(), d->suffix_load_queries_.end(),
                                 [&](auto &value) { return !(d->suffix_load_done_ || value.second(m)); });
  for (auto it = ready_it; it != d->suffix_load_queries_.end(); it++) {
    it->first.set_value(Unit());
  }
  d->suffix_load_queries_.erase(ready_it, d->suffix_load_queries_.end());

  suffix_load_loop(d);
}

void MessagesManager::suffix_load_add_query(Dialog *d,
                                            std::pair<Promise<>, std::function<bool(const Message *)>> query) {
  suffix_load_update_first_message_id(d);
  auto *m = get_message_force(d, d->suffix_load_first_message_id_);
  if (d->suffix_load_done_ || query.second(m)) {
    query.first.set_value(Unit());
  } else {
    d->suffix_load_queries_.emplace_back(std::move(query));
    suffix_load_loop(d);
  }
}

void MessagesManager::suffix_load_till_date(Dialog *d, int32 date, Promise<> promise) {
  LOG(INFO) << "suffix_load_till_date " << d->dialog_id << tag("date", date);
  auto condition = [date](const Message *m) { return m && m->date < date; };
  suffix_load_add_query(d, std::make_pair(std::move(promise), std::move(condition)));
}

void MessagesManager::suffix_load_till_message_id(Dialog *d, MessageId message_id, Promise<> promise) {
  LOG(INFO) << "suffix_load_till_message_id " << d->dialog_id << " " << message_id;
  auto condition = [message_id](const Message *m) { return m && m->message_id.get() < message_id.get(); };
  suffix_load_add_query(d, std::make_pair(std::move(promise), std::move(condition)));
}

Result<ServerMessageId> MessagesManager::get_invoice_message_id(FullMessageId full_message_id) {
  auto message = get_message_force(full_message_id);
  if (message == nullptr) {
    return Status::Error(5, "Message not found");
  }
  if (message->content->get_id() != MessageInvoice::ID) {
    return Status::Error(5, "Message has no invoice");
  }
  auto message_id = full_message_id.get_message_id();
  if (!message_id.is_server()) {
    return Status::Error(5, "Wrong message identifier");
  }
  // TODO need to check that message is not forwarded

  return message_id.get_server_message_id();
}

void MessagesManager::get_payment_form(FullMessageId full_message_id,
                                       Promise<tl_object_ptr<td_api::paymentForm>> &&promise) {
  auto r_message_id = get_invoice_message_id(full_message_id);
  if (r_message_id.is_error()) {
    return promise.set_error(r_message_id.move_as_error());
  }

  td::get_payment_form(r_message_id.ok(), std::move(promise));
}

void MessagesManager::validate_order_info(FullMessageId full_message_id, tl_object_ptr<td_api::orderInfo> order_info,
                                          bool allow_save,
                                          Promise<tl_object_ptr<td_api::validatedOrderInfo>> &&promise) {
  auto r_message_id = get_invoice_message_id(full_message_id);
  if (r_message_id.is_error()) {
    return promise.set_error(r_message_id.move_as_error());
  }

  td::validate_order_info(r_message_id.ok(), std::move(order_info), allow_save, std::move(promise));
}

void MessagesManager::send_payment_form(FullMessageId full_message_id, const string &order_info_id,
                                        const string &shipping_option_id,
                                        const tl_object_ptr<td_api::InputCredentials> &credentials,
                                        Promise<tl_object_ptr<td_api::paymentResult>> &&promise) {
  auto r_message_id = get_invoice_message_id(full_message_id);
  if (r_message_id.is_error()) {
    return promise.set_error(r_message_id.move_as_error());
  }

  td::send_payment_form(r_message_id.ok(), order_info_id, shipping_option_id, credentials, std::move(promise));
}

void MessagesManager::get_payment_receipt(FullMessageId full_message_id,
                                          Promise<tl_object_ptr<td_api::paymentReceipt>> &&promise) {
  auto message = get_message_force(full_message_id);
  if (message == nullptr) {
    return promise.set_error(Status::Error(5, "Message not found"));
  }
  if (message->content->get_id() != MessagePaymentSuccessful::ID) {
    return promise.set_error(Status::Error(5, "Message has wrong type"));
  }
  auto message_id = full_message_id.get_message_id();
  if (!message_id.is_server()) {
    return promise.set_error(Status::Error(5, "Wrong message identifier"));
  }

  td::get_payment_receipt(message_id.get_server_message_id(), std::move(promise));
}

}  // namespace td
