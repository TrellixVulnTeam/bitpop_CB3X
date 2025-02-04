// BitPop browser. Facebook chat integration part.
// Copyright (C) 2014 BitPop AS
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

Chat.ApplicationView = Ember.View.extend({
  classNames: ['root-ember-container', 'chat-dock-wrapper']
});

Chat.Views.Application = Ember.ContainerView.extend({
	controller: Chat.Controllers.application,
    childViews: [
    	Chat.Views.SidebarLogin.create({ elementId: "login-content", classNames: ['card', 'front'] }),
        Chat.Views.SidebarMainUI.create({ classNames: ['card', 'back'] })
    ],
    classNames: ['roster'],
    classNameBindings: ['controller.chatAvailable:flipped']
});
