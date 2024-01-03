/*
 * This file is part of sidplaywx, a GUI player for Commodore 64 SID music files.
 * Copyright (C) 2023-2024 Jasmin Rutic (bytespiller@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see https://www.gnu.org/licenses/gpl-3.0.html
 */

#include "Playlist.h"
#include "../../Config/UIStrings.h"

namespace UIElements
{
	namespace Playlist
	{
		using ColumnId = PlaylistTreeModel::ColumnId;

		Playlist::Playlist(wxPanel* parent, const PlaylistIcons& playlistIcons, Settings::AppSettings& appSettings, unsigned long style) :
			wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, style),
			_model(*new PlaylistTreeModel(playlistIcons)),
			_appSettings(appSettings)
		{
			AssociateModel(&_model);

			_AddBitmapColumn(ColumnId::Icon);
			_AddTextColumn(ColumnId::Title, Strings::PlaylistTree::COLUMN_TITLE);
			_AddTextColumn(ColumnId::Duration, Strings::PlaylistTree::COLUMN_DURATION);
			_AddTextColumn(ColumnId::Author, Strings::PlaylistTree::COLUMN_AUTHOR)->SetHidden(true);
			_AddTextColumn(ColumnId::PlaceholderLast, "");

			Bind(wxEVT_MOUSEWHEEL, &_OverrideScrollWheel, this); // Partial workaround for the smooth scrolling performance issues on MSW (especially with lots of icons in rows).
		}

		wxWindow* Playlist::GetWxWindow()
		{
			return this;
		}

		PlaylistTreeModelNode& Playlist::AddMainSong(const wxString& title, const wxString& filepath, int defaultSubsong, uint_least32_t duration, const wxString& author, PlaylistTreeModelNode::RomRequirement romRequirement, bool playable)
		{
			// Create item
			_model.entries.emplace_back(new PlaylistTreeModelNode(nullptr, title, filepath, defaultSubsong, duration, author, romRequirement, playable));

			// Notify the wx base control of change
			wxDataViewItem childNotify = wxDataViewItem(_model.entries.back().get());
			_model.ItemAdded(wxDataViewItem(0), childNotify);

			// Refresh
			RefreshAuthorColumnVisibility();

			// Return the just-created item for convenience
			return *_model.entries.back().get();
		}

		void Playlist::AddSubsongs(const std::vector<uint_least32_t>& durations, PlaylistTreeModelNode& parent)
		{
			if (durations.size() == 0)
			{
				return;
			}

			wxDataViewItemArray notifyItems(durations.size());

			// Create multiple items at once
			{
				int cnt = 0;
				for (const uint_least32_t duration : durations)
				{
					++cnt;
					PlaylistTreeModelNode& newChildNode = parent.AddChild(new PlaylistTreeModelNode(&parent, wxString::Format("  %s: %s %i", parent.title, Strings::PlaylistTree::SUBSONG, cnt), parent.filepath, cnt, duration, "", parent.romRequirement, parent.IsPlayable()), {});
					notifyItems.Add(wxDataViewItem(&newChildNode));

					// Indicate if default subsong
					if (parent.defaultSubsong == cnt)
					{
						newChildNode.SetIconId(PlaylistIconId::DefaultSubsongIndicator, {});
					}
				}
			}

			// Notify the wx base control of change
			_model.ItemsAdded(wxDataViewItem(&parent), notifyItems);
		}

		void Playlist::Remove(PlaylistTreeModelNode& item)
		{
			void* parent = item.GetParent(); // Can be nullptr if item is a mainsong.

			const PlaylistTreeModelNode* const activeSong = GetActiveSong();
			if (activeSong != nullptr && activeSong->filepath == item.filepath)
			{
				_activeItem.Unset();
			}

			// Find and remove the item from the model (in case of a main song it is removed from the root, in case of a subsong it is removed from its parent main song)
			auto it = std::find_if(_model.entries.begin(), _model.entries.end(), [&item](const PlaylistTreeModelNodePtr& qItemNode) { return qItemNode.get() == &item; });
			if (it != _model.entries.end())
			{
				_model.entries.erase(it); // "item" is now nullptr/invalid, do not access it beyond this point.
			}

			// Notify the wx base control of change
			_model.ItemDeleted(static_cast<wxDataViewItem>(parent), static_cast<wxDataViewItem>(&item));

			// Refresh
			RefreshAuthorColumnVisibility();
		}

		void Playlist::Clear()
		{
			_activeItem.Unset();

			// Clear all entries (entries are unique ptrs so they'll be destroyed since the vector is their owner)
			_model.entries.clear();

			// Notify the wx base control of change
			_model.Cleared();

			// Hide the column
			RefreshAuthorColumnVisibility();
		}

		void Playlist::ExpandSongNode(const PlaylistTreeModelNode& node)
		{
			Expand(PlaylistTreeModel::ModelNodeToTreeItem(node));
		}

		void Playlist::ExpandAll()
		{
			for (const PlaylistTreeModelNodePtr& node : GetSongs())
			{
				Expand(PlaylistTreeModel::ModelNodeToTreeItem(*node.get()));
			}
		}

		void Playlist::CollapseAll()
		{
			for (const PlaylistTreeModelNodePtr& node : GetSongs())
			{
				Collapse(PlaylistTreeModel::ModelNodeToTreeItem(*node.get()));
			}
		}

		bool Playlist::RefreshAuthorColumnVisibility()
		{
			bool shouldHide = true;

			if (_model.entries.size() > 1)
			{
				const wxString& author = _model.entries.front()->author;
				shouldHide = std::all_of(_model.entries.begin(), _model.entries.end(), [&author](const PlaylistTreeModelNodePtr& node)
				{
					return node->author.IsSameAs(author);
				});
			}

			wxDataViewColumn* const col = GetColumn(static_cast<unsigned int>(ColumnId::Author));
			if (col->IsHidden() != shouldHide)
			{
				col->SetHidden(shouldHide);
				OnColumnsCountChanged();
				// Toggle the rightmost placeholder.
				GetColumn(static_cast<unsigned int>(ColumnId::PlaceholderLast))->SetHidden(!shouldHide);
			}

			return shouldHide;
		}

		PlaylistTreeModelNode* Playlist::GetEffectiveInitialSubsong(const PlaylistTreeModelNode& mainSongItem) const
		{
			assert(mainSongItem.type == PlaylistTreeModelNode::ItemType::Song);
			PlaylistTreeModelNode& node = const_cast<PlaylistTreeModelNode&>(mainSongItem);

			// No subsongs, return self.
			if (node.GetSubsongCount() == 0)
			{
				if (node.IsAutoPlayable())
				{
					return &node;
				}

				return nullptr;
			}

			// Try return effective default subsong if it's playable.
			{
				const int preferredStartSubsong = (_appSettings.GetOption(Settings::AppSettings::ID::RepeatModeDefaultSubsong)->GetValueAsBool()) ? node.defaultSubsong : 1;
				PlaylistTreeModelNode* preferredStartSubsongItem = &node.GetSubsong(preferredStartSubsong);

				if (preferredStartSubsongItem != nullptr && preferredStartSubsongItem->IsAutoPlayable())
				{
					return preferredStartSubsongItem;
				}
			}

			// Effective default subsong is not playable, so find the first playable one.
			const PlaylistTreeModelNodePtrArray& subsongs = node.GetChildren();
			auto itSubsong = std::find_if(subsongs.begin(), subsongs.end(), [](const PlaylistTreeModelNodePtr& subsongItem)
			{
				return subsongItem->IsAutoPlayable();
			});

			if (itSubsong == subsongs.end())
			{
				return nullptr;
			}

			return itSubsong->get();
		}

		const PlaylistTreeModelNodePtrArray& Playlist::GetSongs() const
		{
			return _model.entries;
		}

		PlaylistTreeModelNode* Playlist::GetSong(const wxString& filepath) const
		{
			auto itTargetSong = std::find_if(_model.entries.begin(), _model.entries.end(), [&filepath](const PlaylistTreeModelNodePtr& songItem)
			{
				return songItem->filepath.IsSameAs(filepath);
			});

			if (itTargetSong == _model.entries.end())
			{
				return nullptr;
			}

			return itTargetSong->get();
		}

		PlaylistTreeModelNode* Playlist::GetSubsong(const wxString& filepath, int subsong) const
		{
			PlaylistTreeModelNode* mainSongNode = GetSong(filepath);
			if (mainSongNode == nullptr)
			{
				return nullptr;
			}

			if (subsong == 0)
			{
				return GetEffectiveInitialSubsong(*mainSongNode);
			}

			return &mainSongNode->GetSubsong(subsong);
		}

		PlaylistTreeModelNode* Playlist::GetActiveSong() const
		{
			if (_activeItem.IsOk())
			{
				return PlaylistTreeModel::TreeItemToModelNode(_activeItem);
			}

			return nullptr;
		}

		PlaylistTreeModelNode* Playlist::GetNextSong(const PlaylistTreeModelNode& fromSong) const
		{
			const auto itCurrent = std::find_if(_model.entries.begin(), _model.entries.end(), [&fromSong](const PlaylistTreeModelNodePtr& cSongNode) { return cSongNode->filepath.IsSameAs(fromSong.filepath); });
			if (itCurrent == _model.entries.end())
			{
				return nullptr; // Not found (should never happen).
			}

			const auto itNext = std::next(itCurrent);
			if (itNext == _model.entries.end())
			{
				return nullptr; // No further items.
			}

			const PlaylistTreeModelNode* const nextSong = itNext->get(); // Here we know for sure there is at least one more item.
			if (nextSong->IsAutoPlayable())
			{
				return GetEffectiveInitialSubsong(*nextSong);
			}

			return GetNextSong(*nextSong); // Not playable so try the one after it.
		}

		PlaylistTreeModelNode* Playlist::GetNextSong() const
		{
			const PlaylistTreeModelNode* const activeSong = GetActiveSong();
			if (activeSong == nullptr)
			{
				return nullptr;
			}

			return GetNextSong(*activeSong);
		}

		PlaylistTreeModelNode* Playlist::GetPrevSong(const PlaylistTreeModelNode& fromSong) const
		{
			const auto itCurrent = std::find_if(_model.entries.begin(), _model.entries.end(), [&fromSong](const PlaylistTreeModelNodePtr& cSongNode) { return cSongNode->filepath.IsSameAs(fromSong.filepath); });
			if (itCurrent == _model.entries.end() || itCurrent == _model.entries.begin()) // Not found (should never happen) or there aren't any previous items.
			{
				return nullptr;
			}

			const PlaylistTreeModelNode* const prevSong = std::prev(itCurrent)->get(); // Here we know for sure there is at least one previous item.
			if (prevSong->IsAutoPlayable())
			{
				return GetEffectiveInitialSubsong(*prevSong);
			}

			return GetPrevSong(*prevSong); // Not playable so try the one before it.
		}

		PlaylistTreeModelNode* Playlist::GetPrevSong() const
		{
			const PlaylistTreeModelNode* const activeSong = GetActiveSong();
			if (activeSong == nullptr)
			{
				return nullptr;
			}

			return GetPrevSong(*activeSong);
		}

		PlaylistTreeModelNode* Playlist::GetNextSubsong(const PlaylistTreeModelNode& fromSubsong) const
		{
			const PlaylistTreeModelNode* const mainSong = GetSong(fromSubsong.filepath);
			if (mainSong == nullptr)
			{
				return nullptr; // Parent item no longer valid (e.g., removed by user via context menu).
			}

			const int max = GetSong(fromSubsong.filepath)->GetSubsongCount();
			if (fromSubsong.defaultSubsong >= max)
			{
				return nullptr; // No more subsongs.
			}

			PlaylistTreeModelNode* const nextSubsong = GetSubsong(fromSubsong.filepath, fromSubsong.defaultSubsong + 1); // Here we know for sure there is at least one more item.
			if (nextSubsong->IsAutoPlayable())
			{
				return nextSubsong;
			}

			return GetNextSubsong(*nextSubsong); // Not playable so try the one after it.
		}

		PlaylistTreeModelNode* Playlist::GetNextSubsong() const
		{
			const PlaylistTreeModelNode* const activeSong = GetActiveSong();
			if (activeSong == nullptr)
			{
				return nullptr;
			}

			return GetNextSubsong(*activeSong);
		}

		PlaylistTreeModelNode* Playlist::GetPrevSubsong(const PlaylistTreeModelNode& fromSubsong) const
		{
			if (fromSubsong.defaultSubsong <= 1)
			{
				return nullptr; // No more subsongs.
			}

			PlaylistTreeModelNode* const prevSubsong = GetSubsong(fromSubsong.filepath, fromSubsong.defaultSubsong - 1); // Here we know for sure there is at least one previous item.
			if (prevSubsong->IsAutoPlayable())
			{
				return prevSubsong;
			}

			return GetPrevSubsong(*prevSubsong); // Not playable so try the one before it.
		}

		PlaylistTreeModelNode* Playlist::GetPrevSubsong() const
		{
			const PlaylistTreeModelNode* const activeSong = GetActiveSong();
			if (activeSong == nullptr)
			{
				return nullptr;
			}

			return GetPrevSubsong(*activeSong);
		}

		bool Playlist::TrySetActiveSong(const PlaylistTreeModelNode& node, bool autoexpand)
		{
			if (!node.IsPlayable())
			{
				return false;
			}

			// Handle old node
			if (_activeItem.IsOk())
			{
				PlaylistTreeModelNode& oldNode = *GetActiveSong();

				// Collapse old if necessary
				if (autoexpand && oldNode.type == PlaylistTreeModelNode::ItemType::Subsong)
				{
					Collapse(_model.GetParent(_activeItem)); // Collapse parent item.
				}

				// Un-highlight the old node
				oldNode.ResetItemAttr({});

				// Un-highlight the parent item
				PlaylistTreeModelNode* parent = oldNode.GetParent();
				if (parent != nullptr)
				{
					parent->ResetItemAttr({});
				}
			}

			// Highlight new node
			_activeItem = PlaylistTreeModel::ModelNodeToTreeItem(node);
			GetActiveSong()->GetItemAttr({}).SetBold(true);

			// Also highlight the parent node if this is a child node
			if (node.type == PlaylistTreeModelNode::ItemType::Subsong)
			{
				wxDataViewItemAttr& attr = GetActiveSong()->GetParent()->GetItemAttr({});
				attr.SetBold(true);
				attr.SetColour(wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT)); // TODO: define the color in the theme XML instead of here.
			}

			// Expand new if necessary
			if (autoexpand && node.type == PlaylistTreeModelNode::ItemType::Subsong)
			{
				Expand(_model.GetParent(_activeItem)); // Expand parent item.
			}

			Refresh(); // Must be done at the end.
			return true;
		}

		bool Playlist::IsEmpty() const
		{
			return _model.entries.empty();
		}

		void Playlist::SetItemTag(PlaylistTreeModelNode& node, PlaylistTreeModelNode::ItemTag tag, bool force)
		{
			if (!force && !node.IsPlayable())
			{
				return;
			}

			node.SetTag(tag, {});
			if (force)
			{
				node.ResetItemAttr({}); // Reset attributes only on force, so that the context menu actions don't remove the bold styling for hard-selected items.
			}

			// Apply icon & styling attributes
			switch (tag)
			{
				case PlaylistTreeModelNode::ItemTag::Normal:
				{
					// Reset the icon
					{
						const bool itemIsDefaultSubsong = node.type == PlaylistTreeModelNode::ItemType::Subsong && node.defaultSubsong == node.GetParent()->defaultSubsong;
						const PlaylistIconId nodeIconId = (itemIsDefaultSubsong) ? PlaylistIconId::DefaultSubsongIndicator : PlaylistIconId::NoIcon;
						node.SetIconId(nodeIconId, {});
					}

					if (node.romRequirement != PlaylistTreeModelNode::RomRequirement::None)
					{
						// Set chip icon to the main/single song only
						if (node.type == PlaylistTreeModelNode::ItemType::Song)
						{
							node.SetIconId(PlaylistIconId::ChipIcon, {});
						}

						// Apply unplayable styling
						if (!node.IsPlayable())
						{
							const wxColour col = (node.romRequirement == PlaylistTreeModelNode::RomRequirement::BasicRom) ? "#054a80" : "#8a5454"; // TODO: these colors should probably be defined in the theme XML and not hardcoded here.
							node.GetItemAttr({}).SetColour(col);
							node.GetItemAttr({}).SetStrikethrough(true);

							// Apply to any subsongs too
							for (const PlaylistTreeModelNodePtr& subnode : node.GetChildren())
							{
								subnode->GetItemAttr({}).SetColour(col);
								subnode->GetItemAttr({}).SetStrikethrough(true);
							}
						}
					}

					break;
				}
				case PlaylistTreeModelNode::ItemTag::ShortDuration:
				{
					node.SetIconId(PlaylistIconId::SkipShort, {});
					break;
				}
				case PlaylistTreeModelNode::ItemTag::Blacklisted:
				{
					node.SetIconId(PlaylistIconId::RemoveSong, {});
					break;
				}
			}

			_model.ItemChanged(wxDataViewItem(&node)); // Refresh icon immediately.
		}

		bool Playlist::Select(const PlaylistTreeModelNode& node)
		{
			const wxDataViewItem item = PlaylistTreeModel::ModelNodeToTreeItem(node);
			if (item.IsOk())
			{
				wxDataViewCtrl::Select(item);
				return true;
			}

			return false;
		}

		bool Playlist::EnsureVisible(const PlaylistTreeModelNode& node)
		{
			const wxDataViewItem item = PlaylistTreeModel::ModelNodeToTreeItem(node);
			if (item.IsOk())
			{
				wxDataViewCtrl::EnsureVisible(item);
				return true;
			}

			return false;
		}

		wxDataViewColumn* Playlist::_AddBitmapColumn(PlaylistTreeModel::ColumnId columnIndex, wxAlignment align, int flags)
		{
			constexpr int COL_WIDTH = 48; // Col width 48 comes from: 16 * 3 where 16 is playlist icon size and 3 is magic number.
			return AppendBitmapColumn(wxEmptyString, static_cast<unsigned int>(columnIndex), wxDATAVIEW_CELL_INERT, COL_WIDTH, align, flags);
		}

		wxDataViewColumn* Playlist::_AddTextColumn(ColumnId columnIndex, const wxString& title, wxAlignment align, int flags)
		{
			return AppendTextColumn(title, static_cast<unsigned int>(columnIndex), wxDATAVIEW_CELL_INERT, wxCOL_WIDTH_AUTOSIZE, align, flags);
		}

		void Playlist::_OverrideScrollWheel(wxMouseEvent& evt)
		{
			// This method prevents smooth scrolling due to performance issues in the wxDataViewCtrl on MSW.
			if (evt.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL)
			{
				evt.Skip();
				return;
			}

     		const int lines = (evt.GetWheelRotation() / evt.GetWheelDelta()) * evt.GetLinesPerAction();
			DoScroll(GetScrollPos(wxHORIZONTAL), std::max(0, GetScrollPos(wxVERTICAL) - lines)); // Not using the ScrollLines() since it uses the performance-problematic smooth scrolling.
		}
	}
}
