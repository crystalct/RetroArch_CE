/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2013 - Hans-Kristian Arntzen
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "conf/config_file.h"
#include <stdio.h>
#include <stdlib.h>
#include "../compat/getopt_rarch.h"
#include "../boolean.h"
#include "../input/input_common.h"
#include "../general.h"
#include <assert.h>
#include "../compat/posix_string.h"

// Need to be present for build to work, but it's not *really* used.
// Better than having to build special versions of lots of objects with special #ifdefs.
struct settings g_settings;
struct global g_extern;
driver_t driver;

static int g_player = 1;
static int g_joypad = 0;
static char *g_in_path = NULL;
static char *g_out_path = NULL;
static char *g_auto_path = NULL;
static bool g_use_misc = false;

static void print_help(void)
{
   puts("==================");
   puts("retroarch-joyconfig");
   puts("==================");
   puts("Usage: retroarch-joyconfig [ -p/--player <1-8> | -j/--joypad <num> | -i/--input <file> | -o/--output <file> | -h/--help ]");
   puts("");
   puts("-p/--player: Which player to configure for (1 up to and including 8).");
   puts("-j/--joypad: Which joypad to use when configuring (first joypad is 0).");
   puts("-i/--input: Input file to configure with. Binds will be added on or overwritten.");
   puts("\tIf not selected, an empty config will be used as a base.");
   puts("-o/--output: Output file to write to. If not selected, config file will be dumped to stdout.");
   puts("-a/--autoconfig: Outputs an autoconfig file for joypad which was configured.");
   puts("-m/--misc: Also configure various keybinds that are not directly libretro related. These configurations are for player 1 only.");
   puts("-h/--help: This help.");
}

#define MAX_BUTTONS 32
#define MAX_AXES 32
#define MAX_HATS 32
struct poll_data
{
   bool buttons[MAX_BUTTONS];
   int16_t axes[MAX_AXES];
   uint16_t hats[MAX_HATS];
};

static void poll_joypad(const rarch_joypad_driver_t *driver,
      unsigned pad,
      struct poll_data *data)
{
   input_joypad_poll(driver);

   for (unsigned i = 0; i < MAX_BUTTONS; i++)
      data->buttons[i] = input_joypad_button_raw(driver, pad, i);

   for (unsigned i = 0; i < MAX_AXES; i++)
      data->axes[i] = input_joypad_axis_raw(driver, pad, i);

   for (unsigned i = 0; i < MAX_HATS; i++)
   {
      uint16_t hat = 0;
      hat |= input_joypad_hat_raw(driver, pad, HAT_UP_MASK, i)    << HAT_UP_SHIFT;
      hat |= input_joypad_hat_raw(driver, pad, HAT_DOWN_MASK, i)  << HAT_DOWN_SHIFT;
      hat |= input_joypad_hat_raw(driver, pad, HAT_LEFT_MASK, i)  << HAT_LEFT_SHIFT;
      hat |= input_joypad_hat_raw(driver, pad, HAT_RIGHT_MASK, i) << HAT_RIGHT_SHIFT;

      data->hats[i] = hat;
   }
}

static void get_binds(config_file_t *conf, config_file_t *auto_conf, int player, int joypad)
{
   const rarch_joypad_driver_t *driver = input_joypad_init_first();
   if (!driver)
   {
      fprintf(stderr, "Cannot find any valid input driver.\n");
      exit(1);
   }

   if (!driver->query_pad(joypad))
   {
      fprintf(stderr, "Couldn't open joystick #%u.\n", joypad);
      exit(1);
   }

   fprintf(stderr, "Found joypad driver: %s\n", driver->ident);
   const char *joypad_name = input_joypad_name(driver, joypad);
   fprintf(stderr, "Using joypad: %s\n", joypad_name ? joypad_name : "Unknown");

   if (joypad_name && auto_conf)
      config_set_string(auto_conf, "input_device", joypad_name);


   int16_t initial_axes[MAX_AXES] = {0};
   struct poll_data old_poll = {{0}};
   struct poll_data new_poll = {{0}};

   int last_axis   = -1;
   bool block_axis = false;

   poll_joypad(driver, joypad, &old_poll);
   fprintf(stderr, "\nJoypads tend to have stale state after opened.\nPress some buttons and move some axes around to make sure joypad state is reset properly.\nWhen done, press Enter ... ");
   getchar();
   poll_joypad(driver, joypad, &old_poll);

   for (int i = 0; i < MAX_AXES; i++)
   {
      int16_t initial = input_joypad_axis_raw(driver, joypad, i);
      if (abs(initial) < 20000)
         initial = 0;

      // Certain joypads (such as XBox360 controller on Linux) has a default negative axis for shoulder triggers,
      // which makes configuration very awkward.
      // If default negative, we can't trigger on the negative axis, and similar with defaulted positive axes.

      if (initial)
         fprintf(stderr, "Axis %d is defaulted to %s axis value of %d\n", i, initial > 0 ? "positive" : "negative", initial);

      initial_axes[i] = initial;
   }

   fprintf(stderr, "Configuring binds for player #%d on joypad #%d.\n\n",
         player + 1, joypad);

   for (unsigned i = 0; input_config_bind_map[i].valid &&
         (g_use_misc || !input_config_bind_map[i].meta); i++)
   {
      if (i == RARCH_TURBO_ENABLE)
         continue;

      fprintf(stderr, "%s\n", input_config_bind_map[i].desc);

      unsigned player_index = input_config_bind_map[i].meta ? 0 : player;

      for (;;)
      {
         old_poll = new_poll;

         // To avoid pegging CPU.
         // Ideally use an event-based joypad scheme,
         // but it adds far more complexity, so, meh.
         rarch_sleep(10);

         poll_joypad(driver, joypad, &new_poll);
         for (int j = 0; j < MAX_BUTTONS; j++)
         {
            if (new_poll.buttons[j] && !old_poll.buttons[j])
            {
               fprintf(stderr, "\tJoybutton pressed: %u\n", j);
               char key[64];
               snprintf(key, sizeof(key), "%s_%s_btn",
                     input_config_get_prefix(player_index, input_config_bind_map[i].meta), input_config_bind_map[i].base);
               config_set_int(conf, key, j);

               if (auto_conf)
               {
                  snprintf(key, sizeof(key), "input_%s_btn",
                        input_config_bind_map[i].base);
                  config_set_int(auto_conf, key, j);
               }

               goto out;
            }
         }

         for (int j = 0; j < MAX_AXES; j++)
         {
            if (new_poll.axes[j] == old_poll.axes[j])
               continue;

            int16_t value         = new_poll.axes[j];
            bool same_axis        = last_axis == j;
            bool require_negative = initial_axes[j] > 0;
            bool require_positive = initial_axes[j] < 0;

            // Block the axis config until we're sure axes have returned to their neutral state.
            if (same_axis)
            {
               if (abs(value) < 10000 ||
                     (require_positive && value < 0) ||
                     (require_negative && value > 0))
                  block_axis = false;
            }

            // If axes are in their neutral state, we can't allow it.
            if (require_negative && value >= 0)
               continue;
            if (require_positive && value <= 0)
               continue;

            if (block_axis)
               continue;

            if (abs(value) > 20000)
            {
               last_axis = j;
               fprintf(stderr, "\tJoyaxis moved: Axis %d, Value %d\n", j, value);

               char buf[8];
               snprintf(buf, sizeof(buf),
                     value > 0 ? "+%d" : "-%d", j);

               char key[64];
               snprintf(key, sizeof(key), "%s_%s_axis",
                     input_config_get_prefix(player_index, input_config_bind_map[i].meta), input_config_bind_map[i].base);

               config_set_string(conf, key, buf);

               if (auto_conf)
               {
                  snprintf(key, sizeof(key), "input_%s_axis",
                        input_config_bind_map[i].base);
                  config_set_string(auto_conf, key, buf);
               }

               block_axis = true;
               goto out;
            }
         }

         for (int j = 0; j < MAX_HATS; j++)
         {
            const char *quark  = NULL;
            uint16_t value     = new_poll.hats[j];
            uint16_t old_value = old_poll.hats[j];

            if ((value & HAT_UP_MASK) && !(old_value & HAT_UP_MASK))
               quark = "up";
            else if ((value & HAT_LEFT_MASK) && !(old_value & HAT_LEFT_MASK))
               quark = "left";
            else if ((value & HAT_RIGHT_MASK) && !(old_value & HAT_RIGHT_MASK))
               quark = "right";
            else if ((value & HAT_DOWN_MASK) && !(old_value & HAT_DOWN_MASK))
               quark = "down";

            if (quark)
            {
               fprintf(stderr, "\tJoyhat moved: Hat %d, direction %s\n", j, quark);
               char buf[16];
               snprintf(buf, sizeof(buf), "h%d%s", j, quark);

               char key[64];
               snprintf(key, sizeof(key), "%s_%s_btn",
                     input_config_get_prefix(player_index, input_config_bind_map[i].meta), input_config_bind_map[i].base);

               config_set_string(conf, key, buf);

               if (auto_conf)
               {
                  snprintf(key, sizeof(key), "input_%s_btn",
                        input_config_bind_map[i].base);
                  config_set_string(auto_conf, key, buf);
               }

               goto out;
            }
         }
      }
out:
      old_poll = new_poll;
   }
}

static void parse_input(int argc, char *argv[])
{
   char optstring[] = "i:o:a:p:j:hm";
   struct option opts[] = {
      { "input", 1, NULL, 'i' },
      { "output", 1, NULL, 'o' },
      { "autoconfig", 1, NULL, 'a' },
      { "player", 1, NULL, 'p' },
      { "joypad", 1, NULL, 'j' },
      { "help", 0, NULL, 'h' },
      { "misc", 0, NULL, 'm' },
      { NULL, 0, NULL, 0 }
   };

   int option_index = 0;
   for (;;)
   {
      int c = getopt_long(argc, argv, optstring, opts, &option_index);
      if (c == -1)
         break;

      switch (c)
      {
         case 'h':
            print_help();
            exit(0);

         case 'i':
            g_in_path = strdup(optarg);
            break;

         case 'o':
            g_out_path = strdup(optarg);
            break;

         case 'a':
            g_auto_path = strdup(optarg);
            break;

         case 'm':
            g_use_misc = true;
            break;

         case 'j':
            g_joypad = strtol(optarg, NULL, 0);
            if (g_joypad < 0)
            {
               fprintf(stderr, "Joypad number can't be negative.\n");
               exit(1);
            }
            break;

         case 'p':
            g_player = strtol(optarg, NULL, 0);
            if (g_player < 1)
            {
               fprintf(stderr, "Player number must be at least 1.\n");
               exit(1);
            }
            else if (g_player > MAX_PLAYERS)
            {
               fprintf(stderr, "Player number must be from 1 to %d.\n", MAX_PLAYERS);
               exit(1);
            }
            break;

         default:
            break;
      }
   }

   if (optind < argc)
   {
      print_help();
      exit(1);
   }

}

// Need SDL_main on OSX.
#ifndef __APPLE__
#undef main
#endif

int main(int argc, char *argv[])
{
   parse_input(argc, argv);

   config_file_t *conf = config_file_new(g_in_path);
   if (!conf)
   {
      fprintf(stderr, "Couldn't open config file ...\n");
      return 1;
   }

   const char *index_list[] = { 
      "input_player1_joypad_index", 
      "input_player2_joypad_index", 
      "input_player3_joypad_index", 
      "input_player4_joypad_index", 
      "input_player5_joypad_index",
      "input_player6_joypad_index",
      "input_player7_joypad_index",
      "input_player8_joypad_index",
   };

   config_set_int(conf, index_list[g_player - 1], g_joypad);

   config_file_t *auto_conf = NULL;
   if (g_auto_path)
      auto_conf = config_file_new(NULL);

   get_binds(conf, auto_conf, g_player - 1, g_joypad);
   config_file_write(conf, g_out_path);
   config_file_free(conf);
   if (auto_conf)
   {
      fprintf(stderr, "Writing autoconfig profile to: %s.\n", g_auto_path);
      config_file_write(auto_conf, g_auto_path);
      config_file_free(auto_conf);
   }

   free(g_in_path);
   free(g_out_path);
   free(g_auto_path);
   return 0;
}
