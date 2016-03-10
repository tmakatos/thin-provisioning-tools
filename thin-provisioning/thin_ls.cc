// Copyright (C) 2015 Red Hat, Inc. All rights reserved.
//
// This file is part of the thin-provisioning-tools source.
//
// thin-provisioning-tools is free software: you can redistribute it
// and/or modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// thin-provisioning-tools is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with thin-provisioning-tools.  If not, see
// <http://www.gnu.org/licenses/>.

#include <fstream>
#include <iostream>
#include <getopt.h>
#include <libgen.h>

#include "base/disk_units.h"
#include "base/grid_layout.h"
#include "boost/lexical_cast.hpp"
#include "boost/optional.hpp"
#include "boost/range.hpp"
#include "persistent-data/file_utils.h"
#include "thin-provisioning/commands.h"
#include "thin-provisioning/human_readable_format.h"
#include "thin-provisioning/metadata.h"
#include "thin-provisioning/metadata_dumper.h"
#include "thin-provisioning/xml_format.h"
#include "version.h"

using namespace base;
using namespace boost;
using namespace persistent_data;
using namespace std;
using namespace thin_provisioning;


//----------------------------------------------------------------

namespace {
	class mapping_set {
	public:
		mapping_set()
		: bits_(10240, false) {
		}

		enum block_state {
			UNMAPPED,
			EXCLUSIVE,
			SHARED
		};

		void inc(block_address b) {
			if (get_bit(b * 2))
				set_bit(b * 2 + 1, true); // shared
			else
				set_bit(b * 2, true); // exclusive
		}

		block_state get_state(block_address b) const {
			if (get_bit(b * 2)) {
				if (get_bit(b * 2 + 1))
					return SHARED;
				else
					return EXCLUSIVE;
			} else
				return UNMAPPED;
		}

	private:
		void ensure_size(block_address bit) const {
			if (bit >= bits_.size()) {
				unsigned new_size = bits_.size() * 2;
				while (new_size < bit)
					new_size *= 2;

				bits_.resize(new_size, false);
			}
		}

		bool get_bit(block_address bit) const {
			ensure_size(bit);
			return bits_[bit];
		}

		void set_bit(block_address bit, bool v) {
			ensure_size(bit);
			bits_[bit] = v;
		}

		mutable vector<bool> bits_;
	};

	//------------------------------------------------

	enum output_field {
		DEV_ID,
		MAPPED_BLOCKS,
		EXCLUSIVE_BLOCKS,
		SHARED_BLOCKS,

		MAPPED_SECTORS,
		EXCLUSIVE_SECTORS,
		SHARED_SECTORS,

		MAPPED_BYTES,
		EXCLUSIVE_BYTES,
		SHARED_BYTES,

		MAPPED,
		EXCLUSIVE,
		SHARED,

		TRANSACTION_ID,
		CREATION_TIME,
		SNAPSHOT_TIME	// make sure this is always the last one
	};

	char const *field_names[] = {
		"DEV",
		"MAPPED_BLOCKS",
		"EXCLUSIVE_BLOCKS",
		"SHARED_BLOCKS",

		"MAPPED_SECTORS",
		"EXCLUSIVE_SECTORS",
		"SHARED_SECTORS",

		"MAPPED_BYTES",
		"EXCLUSIVE_BYTES",
		"SHARED_BYTES",

		"MAPPED",
		"EXCLUSIVE",
		"SHARED",

		"TRANSACTION",
		"CREATE_TIME",
		"SNAP_TIME"
	};

	output_field string_to_field(string const &str) {
		for (unsigned i = 0; i < size(field_names); i++)
			if (str == field_names[i])
				return static_cast<output_field>(i);

		throw runtime_error("unknown field");
		return DEV_ID;
	}

	string field_to_string(output_field const &f) {
		return field_names[static_cast<unsigned>(f)];
	}

	void print_headers(grid_layout &out, vector<output_field> const &fields) {
		vector<output_field>::const_iterator it;
		for (it = fields.begin(); it != fields.end(); ++it)
			out.field(field_to_string(*it));

		out.new_row();
	}

	//------------------------------------------------

	struct flags {
		flags()
			: use_metadata_snap(false),
			  headers(true) {

			fields.push_back(DEV_ID);
			fields.push_back(MAPPED);
			fields.push_back(CREATION_TIME);
			fields.push_back(SNAPSHOT_TIME);
		}

		bool use_metadata_snap;
		bool headers;
		vector<output_field> fields;
	};

	//------------------------------------------------

	class mapping_pass1 : public mapping_tree_detail::mapping_visitor {
	public:
		mapping_pass1(mapping_set &mappings)
		: mappings_(mappings) {
		}

		virtual void visit(btree_path const &path, mapping_tree_detail::block_time const &bt) {
			mappings_.inc(bt.block_);
		}

	private:
		mapping_set &mappings_;
	};

	class mapping_pass2 : public mapping_tree_detail::mapping_visitor {
	public:
		mapping_pass2(mapping_set const &mappings)
		: mappings_(mappings),
		  exclusives_(0) {
		}

		virtual void visit(btree_path const &path, mapping_tree_detail::block_time const &bt) {
			if (mappings_.get_state(bt.block_) == mapping_set::EXCLUSIVE)
				exclusives_++;
		}

		block_address get_exclusives() const {
			return exclusives_;
		}

	private:
		mapping_set const &mappings_;
		block_address exclusives_;
	};

	void raise_metadata_damage() {
		throw std::runtime_error("metadata contains errors (run thin_check for details).");
	}

	class fatal_mapping_damage : public mapping_tree_detail::damage_visitor {
	public:
		virtual void visit(mapping_tree_detail::missing_devices const &d) {
			raise_metadata_damage();
		}

		virtual void visit(mapping_tree_detail::missing_mappings const &d) {
			raise_metadata_damage();
		}
	};

	void pass1(metadata::ptr md, mapping_set &mappings, uint64_t dev_id) {
		dev_tree::key k = {dev_id};
		optional<uint64_t> dev_root = md->mappings_top_level_->lookup(k);

		if (!dev_root)
			throw runtime_error("couldn't find mapping tree root");

		single_mapping_tree dev_mappings(*md->tm_, *dev_root,
				   mapping_tree_detail::block_traits::ref_counter(md->tm_->get_sm()));

		mapping_pass1 pass1(mappings);
		fatal_mapping_damage dv;
		walk_mapping_tree(dev_mappings, pass1, dv);
	}


	block_address count_exclusives(metadata::ptr md, mapping_set const &mappings, uint64_t dev_id) {
		dev_tree::key k = {dev_id};
		optional<uint64_t> dev_root = md->mappings_top_level_->lookup(k);

		if (!dev_root)
			throw runtime_error("couldn't find mapping tree root");

		single_mapping_tree dev_mappings(*md->tm_, *dev_root,
				   mapping_tree_detail::block_traits::ref_counter(md->tm_->get_sm()));

		mapping_pass2 pass2(mappings);
		fatal_mapping_damage dv;
		walk_mapping_tree(dev_mappings, pass2, dv);
		return pass2.get_exclusives();
	}

	//------------------------------------------------

 	typedef map<block_address, device_tree_detail::device_details> dd_map;

	class details_extractor : public device_tree_detail::device_visitor {
	public:
		void visit(block_address dev_id, device_tree_detail::device_details const &dd) {
			dd_.insert(make_pair(dev_id, dd));
		}

		dd_map const &get_details() const {
			return dd_;
		}

	private:
		dd_map dd_;
	};

	struct fatal_details_damage : public device_tree_detail::damage_visitor {
		void visit(device_tree_detail::missing_devices const &d) {
			raise_metadata_damage();
		}
	};

	device_tree_detail::damage_visitor::ptr details_damage_policy() {
		typedef device_tree_detail::damage_visitor::ptr dvp;
		return dvp(new fatal_details_damage());
	}

	//------------------------------------------------

	bool pass1_needed(vector<output_field> const &fields) {
		vector<output_field>::const_iterator it;
		for (it = fields.begin(); it != fields.end(); ++it) {
			if (*it == EXCLUSIVE_BLOCKS ||
			    *it == SHARED_BLOCKS ||
			    *it == EXCLUSIVE_SECTORS ||
			    *it == SHARED_SECTORS ||
			    *it == EXCLUSIVE_BYTES ||
			    *it == SHARED_BYTES ||
			    *it == EXCLUSIVE ||
			    *it == SHARED)
				return true;
		}

		return false;
	}

	void ls_(string const &path, ostream &out, struct flags &flags) {
		grid_layout grid;

		block_manager<>::ptr bm(open_bm(path, block_manager<>::READ_ONLY,
						!flags.use_metadata_snap));
		metadata::ptr md;

		if (flags.use_metadata_snap)
			md.reset(new metadata(bm, optional<block_address>()));
		else
			md.reset(new metadata(bm));

		block_address block_size = md->sb_.data_block_size_;

		details_extractor de;
		device_tree_detail::damage_visitor::ptr dd_policy(details_damage_policy());
		walk_device_tree(*md->details_, de, *dd_policy);

		mapping_set mappings;
		dd_map::const_iterator it;
		dd_map const &map = de.get_details();

		bool some_exclusive_fields = pass1_needed(flags.fields);
		if (some_exclusive_fields) {
			for (it = map.begin(); it != map.end(); ++it)
				pass1(md, mappings, it->first);
		}

		if (flags.headers)
			print_headers(grid, flags.fields);

		for (it = map.begin(); it != map.end(); ++it) {
			vector<output_field>::const_iterator f;

			block_address exclusive = 0;

			if (some_exclusive_fields)
				exclusive = count_exclusives(md, mappings, it->first);

			for (f = flags.fields.begin(); f != flags.fields.end(); ++f) {
				switch (*f) {
				case DEV_ID:
					grid.field(it->first);
					break;

				case MAPPED_BLOCKS:
					grid.field(it->second.mapped_blocks_);
					break;

				case EXCLUSIVE_BLOCKS:
					grid.field(exclusive);
					break;

				case SHARED_BLOCKS:
					grid.field(it->second.mapped_blocks_ - exclusive);
					break;

				case MAPPED_SECTORS:
					grid.field(it->second.mapped_blocks_ * block_size);
					break;

				case EXCLUSIVE_SECTORS:
					grid.field(exclusive * block_size);
					break;

				case SHARED_SECTORS:
					grid.field((it->second.mapped_blocks_ - exclusive) * block_size);
					break;

				case MAPPED_BYTES:
					grid.field(it->second.mapped_blocks_ * block_size *
					           disk_unit_multiplier(UNIT_SECTOR));
					break;

				case EXCLUSIVE_BYTES:
					grid.field(exclusive * block_size *
					           disk_unit_multiplier(UNIT_SECTOR));
					break;

				case SHARED_BYTES:
					grid.field((it->second.mapped_blocks_ - exclusive) * block_size *
					           disk_unit_multiplier(UNIT_SECTOR));
					break;

				case MAPPED:
					grid.field(
						format_disk_unit(it->second.mapped_blocks_ * block_size,
								 UNIT_SECTOR));
					break;

				case EXCLUSIVE:
					grid.field(
						format_disk_unit(exclusive * block_size,
								 UNIT_SECTOR));
					break;

				case SHARED:
					grid.field(
						format_disk_unit((it->second.mapped_blocks_ - exclusive) *
								 block_size, UNIT_SECTOR));
					break;

				case TRANSACTION_ID:
					grid.field(it->second.transaction_id_);
					break;

				case CREATION_TIME:
					grid.field(it->second.creation_time_);
					break;

				case SNAPSHOT_TIME:
					grid.field(it->second.snapshotted_time_);
				}
			}
			grid.new_row();
		}

		grid.render(out);
	}

	int ls(string const &path, ostream &out, struct flags &flags) {
		try {
			ls_(path, out, flags);

		} catch (std::exception &e) {
			cerr << e.what() << endl;
			return 1;
		}

		return 0;
	}
}

//----------------------------------------------------------------

thin_ls_cmd::thin_ls_cmd()
	: command("thin_ls")
{
}

void
thin_ls_cmd::usage(std::ostream &out) const
{
	out << "Usage: " << get_name() << " [options] {metadata device}\n"
	    << "Options:\n"
	    << "  {-h|--help}\n"
	    << "  {-m|--metadata-snap}\n"
	    << "  {--no-headers}\n"
	    << "  {-o|--format <fields>}\n"
	    << "  {-V|--version}\n\n"
	    << "where <fields> is a comma separated list from:\n";

	for (unsigned i = 0; i <= static_cast<unsigned>(SNAPSHOT_TIME); i++)
            out << "  " << field_to_string(static_cast<output_field>(i)) << "\n";
}

vector<output_field> parse_fields(string const &str)
{
	vector<output_field> fields;
	stringstream in(str);
	string item;

	while (getline(in, item, ','))
		fields.push_back(string_to_field(item));

	return fields;
}

int
thin_ls_cmd::run(int argc, char **argv)
{
	int c;
	struct flags flags;
	const char shortopts[] = "ho:m::V";

	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h'},
		{ "metadata-snap", no_argument, NULL, 'm' },
		{ "version", no_argument, NULL, 'V'},
		{ "format", required_argument, NULL, 'o' },
		{ "no-headers", no_argument, NULL, 1 },
		{ NULL, no_argument, NULL, 0 }
	};

	while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
		switch(c) {
		case 'h':
			usage(cout);
			return 0;

		case 'm':
			flags.use_metadata_snap = true;
			break;

		case 'o':
			flags.fields = parse_fields(optarg);
			break;

		case 'V':
			cout << THIN_PROVISIONING_TOOLS_VERSION << endl;
			return 0;

		case 1:
			flags.headers = false;
			break;

		default:
			usage(cerr);
			return 1;
		}
	}

	if (argc == optind) {
		cerr << "No input file provided." << endl;
		usage(cerr);
		return 1;
	}

	return ls(argv[optind], cout, flags);
}

//----------------------------------------------------------------
