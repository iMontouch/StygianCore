--[[
-- ################################################################################### --
--  ____    __                                         ____                           
-- /\  _`\ /\ \__                  __                 /\  _`\                         
-- \ \,\L\_\ \ ,_\  __  __     __ /\_\     __      ___\ \ \/\_\    ___   _ __    __   
--  \/_\__ \\ \ \/ /\ \/\ \  /'_ `\/\ \  /'__`\  /' _ `\ \ \/_/_  / __`\/\`'__\/'__`\ 
--    /\ \L\ \ \ \_\ \ \_\ \/\ \L\ \ \ \/\ \L\.\_/\ \/\ \ \ \L\ \/\ \L\ \ \ \//\  __/ 
--    \ `\____\ \__\\/`____ \ \____ \ \_\ \__/.\_\ \_\ \_\ \____/\ \____/\ \_\\ \____\
--     \/_____/\/__/ `/___/> \/___L\ \/_/\/__/\/_/\/_/\/_/\/___/  \/___/  \/_/ \/____/
--                      /\___/ /\____/                                                
--                      \/__/  \_/__/          http://stygianthebest.github.io                                         
-- 
-- ################################################################################### --

-- WORLD: Eluna GameObject Gossip
-- Gives a game object a gossip menu with options

-- Eluna LUA Engine
-- http://www.elunaengine.com/index.html

  This sword has always been in the game. It was made unclickable in a later xpac.
  Perhaps it had a future.. once.

// Testing
  .tele deadkingscrypt, then click on the sword

// Example Spells
	25823 -- Spotlight
	62296 -- Red Spotlight
	16609 -- Lightning
	56525 -- Blessing of Kings

// Example Models
	  999 -- Crab
	 2177 -- Cockroach	
	 9786 -- Skeleton
	18043 -- Spider
	11828 -- Worm
  
// Gossip Icons
	GOSSIP_ICON_CHAT                = 0,      //white chat bubble
	GOSSIP_ICON_VENDOR              = 1,      //brown bag
	GOSSIP_ICON_TAXI                = 2,      //flight
	GOSSIP_ICON_TRAINER             = 3,      //book
	GOSSIP_ICON_INTERACT_1          = 4,      //interaction wheel
	GOSSIP_ICON_INTERACT_2          = 5,      //interaction wheel
	GOSSIP_ICON_MONEY_BAG           = 6,      //brown bag with yellow dot
	GOSSIP_ICON_TALK                = 7,      //white chat bubble with black dots
	GOSSIP_ICON_TABARD              = 8,      //tabard
	GOSSIP_ICON_BATTLE              = 9,      //two swords
	GOSSIP_ICON_DOT                 = 10,     //yellow dot


-- ################################################################################### --

--]]

-- GameObject Entry
local GameObjectID = 144181 

-- Interact With Game Object
local function OnGossipHello(event, player, object)
-- Player:GossipMenuAddItem(icon, msg, sender, intid)
   player:GossipMenuAddItem(9, "Pry the sword from the king's bony fingers.", 1, 1)
   player:GossipMenuAddItem(0, "Say a prayer for the dead king.", 1, 2)
   player:GossipSendMenu(1, object)
end

-- ---------------------------------------------------------------------------------------
-- Choose A Gossip Option
local function OnGossipSelect(event, player, object, sender, intid, code, menu_id)
	if (intid == 1) then
		player:CastSpell(player, 16609) -- Lightning
		player:SetDisplayId(9786) -- Skeleton
		player:GossipComplete()
	elseif (intid == 2) then
	    player:DeMorph()
		player:CastSpell(player, 56525) -- Blessing of Kings
		player:CastSpell(player, 25823) -- Spotlight
        player:GossipComplete()
    end
end

-- Register Gossip Object
RegisterGameObjectGossipEvent(GameObjectID, 1, OnGossipHello)
RegisterGameObjectGossipEvent(GameObjectID, 2, OnGossipSelect)

--[[ ################################################################################### --]]