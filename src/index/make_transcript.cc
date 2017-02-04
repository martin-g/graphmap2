// #include "make_transcript.h"
#include "index_spaced_hash_fast.h"

bool IndexSpacedHashFast::is_transcriptome() const {
  return is_transcriptome_;
}

int IndexSpacedHashFast::LoadOrGenerate(std::string reference_path, std::string out_index_path, bool verbose) {
  FILE *fp=NULL;
  fp = fopen(out_index_path.c_str(), "r");
  if (fp != NULL) {
    fclose(fp);
    int ret_load_from_file = this->LoadFromFile(out_index_path);

    if (ret_load_from_file != 0 || is_transcriptome_ == true) {
      if (ret_load_from_file != 0 && verbose == true) {
        LOG_ALL("Index needs to be rebuilt. It was generated using an older version.\n");
        LOG_DEBUG("ret_load_from_file = %d\n", ret_load_from_file);
      }
      if (is_transcriptome_ == true && verbose == true) {
        LOG_ALL("Existing index is a transcriptome, and you are trying to map to a genome. Index needs to be rebuilt.\n");
      }

      is_transcriptome_ = false;
      GenerateFromFile(reference_path);

      if (verbose == true) { LOG_ALL("Storing new index to file '%s'...\n", out_index_path.c_str()); }
      StoreToFile(out_index_path);
      if (verbose == true) { LOG_ALL("New index stored.\n"); }

    }
  } else {
    if (verbose == true) { LOG_ALL("Started generating new index from file '%s'...\n", reference_path.c_str()); }

    is_transcriptome_ = false;
    GenerateFromFile(reference_path);

    if (verbose == true) { LOG_ALL("Storing new index to file '%s'...\n", out_index_path.c_str()); }
    StoreToFile(out_index_path);
    if (verbose == true) { LOG_ALL("New index stored.\n"); }
  }

  return 0;
}

 int IndexSpacedHashFast::LoadOrGenerateTranscriptome(std::string reference_path, std::string gtf_path, std::string out_index_path, bool verbose) {
  FILE *fp=NULL;
  fp = fopen(out_index_path.c_str(), "r");
  if (fp != NULL) {
    fclose(fp);

    LoadGTFInfo_(gtf_path);

    int ret_load_from_file = LoadFromFile(out_index_path);

    if (ret_load_from_file == 0 && is_transcriptome_ == true) {  // Everything went fine. Prepare headers for SAM output.
      // Verbose output for debugging.
      LOG_ALL("Loading the genomic sequences.\n");
      SequenceFile sequences(reference_path);
      genome_id_to_len_.clear();
      HashGenomeLengths_(sequences, genome_id_to_len_);

    } else {  // Something went wrong, generate the transcriptome again. This will prepare the headers as well.
      if (ret_load_from_file != 0 && verbose == true) {
        LOG_ALL("Index needs to be rebuilt. It was generated using an older version.\n");
        LOG_DEBUG("ret_load_from_file = %d\n", ret_load_from_file);
      }
      if (is_transcriptome_ == false && verbose == true) {
        LOG_ALL("Existing index is a genome, and you are trying to map to a transcriptome. Index needs to be rebuilt.\n");
      }

      GenerateTranscriptomeFromFile(reference_path, gtf_path);

      if (verbose == true) { LOG_ALL("Storing new index to file '%s'...\n", out_index_path.c_str()); }
      StoreToFile(out_index_path);
      if (verbose == true) { LOG_ALL("New index stored.\n"); }
    }
  } else {
    if (verbose == true) { LOG_ALL("Started generating new index from file '%s'...\n", reference_path.c_str()); }

    GenerateTranscriptomeFromFile(reference_path, gtf_path);

    if (verbose == true) { LOG_ALL("Storing new index to file '%s'...\n", out_index_path.c_str()); }
    StoreToFile(out_index_path);
    if (verbose == true) { LOG_ALL("New index stored.\n"); }
  }

  return 0;
}

int IndexSpacedHashFast::LoadGTFInfo_(const std::string &gtf_path) {
  genome_id_to_trans_id_.clear();
  trans_id_to_genome_id_.clear();
  trans_id_to_exons_.clear();
  trans_id_to_regions_.clear();
  is_transcriptome_ = false;

  // Parse the GTF for exons.
  ParseExons_(gtf_path, genome_id_to_trans_id_, trans_id_to_genome_id_, trans_id_to_exons_);

  trans_id_to_regions_.clear();
  MakeRegions_(trans_id_to_exons_, trans_id_to_regions_);

  return 0;
}

int IndexSpacedHashFast::GenerateTranscriptomeFromFile(const std::string &sequence_file_path, const std::string &gtf_path, bool verbose) {
  LogSystem::GetInstance().Log(VERBOSE_LEVEL_MED_DEBUG | VERBOSE_LEVEL_HIGH_DEBUG, true, FormatString("Loading reference from file, and creating a transcriptome index.\n"), "GenerateFromFile");

  LoadGTFInfo_(gtf_path);

  LOG_ALL("Loading the genomic sequences.\n");
  SequenceFile sequences(sequence_file_path);

  // Construct transcriptome sequences
  LOG_ALL("Constructing the transcriptome sequences.\n");
  SequenceFile transcript_sequences;
  MakeTranscript_(genome_id_to_trans_id_, trans_id_to_exons_, sequences, transcript_sequences);
  LOG_ALL("In total, there are %ld transcripts.\n", transcript_sequences.get_sequences().size());

  // Sanity check.
  if (transcript_sequences.get_sequences().size() == 0) {
    return 1;
  }

  genome_id_to_len_.clear();
  HashGenomeLengths_(sequences, genome_id_to_len_);

  is_transcriptome_ = true;

//  transcript_sequences.WriteFASTA("test_transcript.fasta");

  return GenerateFromSequenceFile(transcript_sequences);
}

std::string IndexSpacedHashFast::GenerateSAMHeaders() {
  std::stringstream ss;

  if (is_transcriptome_ == false) {
    for (int64_t reference_id=0; reference_id<((int64_t) get_num_sequences_forward()); reference_id++) {
      std::string reference_header = TrimToFirstSpace(std::string(get_headers()[reference_id]));
      uint64_t reference_length = (uint64_t) get_reference_lengths()[reference_id];
      ss << "@SQ\t" << "SN:" << reference_header << "\t" << "LN:" << reference_length << "\n";
    }

  } else {
    for (auto& g: genome_id_to_len_) {
      ss << "@SQ\t" << "SN:" << TrimToFirstSpace(g.first) << "\t" << "LN:" << g.second << "\n";
    }
  }

  return ss.str();
}

int IndexSpacedHashFast::MakeRegions_(const std::map<std::string, std::vector<std::pair<int64_t, int64_t>>> &trans_id_to_exons,
                 std::map<std::string, std::vector<std::pair<int64_t, int64_t>>> &trans_id_to_regions) const {
  for (auto& it: trans_id_to_exons) {
    auto& tid = it.first;
    auto& exons = it.second;
    if (exons.size() == 0) { continue; }
    int64_t start = exons[0].first;
    int64_t end = exons[0].second;

    trans_id_to_regions[tid] = std::vector<std::pair<int64_t, int64_t>>();
//    std::vector<std::pair<int64_t, int64_t>> regions;
    auto& regions = trans_id_to_regions[tid];

    for (auto& exon: exons) {
      if (exon.first <= end) {
        end = std::max(end, exon.second);
      } else {
        regions.emplace_back(std::make_pair(start, end));
        start = exon.first;
        end = exon.second;
      }
    }

    if (regions.size() == 0 || (regions.back().first != start || regions.back().second != end)) {
      regions.emplace_back(std::make_pair(start, end));
    }
  }

  return 0;
}

int IndexSpacedHashFast::MakeTranscript_(const std::map<std::string, std::vector<std::pair<std::string, char>>> &genome_id_to_trans_id,
                                         const std::map<std::string, std::vector<std::pair<int64_t, int64_t>>> &trans_id_to_exons,
                                         const SequenceFile &references,
                                         SequenceFile &transcripts) const {
	transcripts.Clear();
	int64_t refN = references.get_sequences().size();
	int64_t id = 1;
	for(int64_t i = 0; i < refN; i++) {
		auto seq = references.get_sequences()[i];
		auto seqName = getSequenceName_(*seq);

		auto trans_it = genome_id_to_trans_id.find(seqName);
		if (trans_it == genome_id_to_trans_id.end()) {
		  continue;
		}

		for(auto &trans : trans_it->second) {
			auto exon_it = trans_id_to_exons.find(trans.first);
			if (exon_it == trans_id_to_exons.end()) {
			  continue;
			}

      std::string transSeq = "";
			for(auto &coord : exon_it->second) {
				for(int64_t j = coord.first - 1; j < coord.second; j++) {
					transSeq += seq->get_data()[j];
				}
			}
			if(transSeq == "") continue;
			SingleSequence *s = new SingleSequence;

//			auto seqLen = transSeq.size();
//			int8_t *seq = new int8_t[seqLen];
//			std::vector<int8_t> seq(seqLen);
//			std::copy(transSeq.begin(), transSeq.end(), &seq[0]);

//			auto headerLen = trans.first.size();
//			char *header = new char[headerLen];
//			std::copy(trans.first.begin(), trans.first.end(), header);

			s->InitHeaderAndDataFromAscii((char *) &trans.first[0], trans.first.size(), (int8_t *) &transSeq[0], transSeq.length(), id++);
			if(trans.second == '-') {
				s->ReverseComplement();
			}
			transcripts.AddSequence(s, true);
			// outputSeq(header, headerLen, s->get_data(), seqLen);
		}
	}
	return 0;
}

int IndexSpacedHashFast::ParseExons_(const std::string &annotations_path,
                                     std::map<std::string, std::vector<std::pair<std::string, char>>> &genomeToTrans,
                                     std::map<std::string, std::pair<std::string, char>> &transIdToGenomeId,
                                     std::map<std::string, std::vector<std::pair<int64_t, int64_t>>> &transToExons) const {
	std::ifstream annotations(annotations_path);
	if(!annotations.is_open()) {
      FATAL_REPORT(ERR_OPENING_FILE, "File path: '%s'.", annotations_path.c_str());
      return 1;
    }
	std::string line;
	while(getline(annotations, line)) {
		auto fields = split_(line, '\t');
		if(fields.size() < 9 || fields[2] != "exon") {
			continue;
		}
		// tid will internally have the chromosome name appended to the back (format: "tid_chr").
		// This is to handle a special case when GTF file is faulty and there are multiple same TIDs
		// on several different chromosomes, which shouldn't be possible.
    std::string chr_name = split_(fields[0], ' ')[0];
		std::string tid = getTID_(chr_name, fields[8]);  // Transcrip ID (name)
		if(transToExons[tid].empty()) {
		  // Field 6 (fields[6]) is the strand (either '+' or '-').
		  char orient = fields[6][0];
			genomeToTrans[chr_name].push_back(std::make_pair(tid, orient));
	    transIdToGenomeId[tid] = std::make_pair(chr_name, orient);
		}
		int64_t left, right;
	  std::stringstream ss(fields[3] + " " + fields[4]);
		ss >> left >> right;
		transToExons[tid].push_back(std::make_pair(left, right));
	}

	for (auto& trans: transToExons) {
	  std::sort(trans.second.begin(), trans.second.end(),
	            [](const std::pair<int64_t, int64_t> &a, const std::pair<int64_t, int64_t> &b){ return (a.first < b.first); });
	}

	return 0;
}

std::string IndexSpacedHashFast::getTID_(const std::string &chr_name, const std::string &attributes) const {
	for(auto s : split_(attributes, ';')) {
		s = trim_(s);
		auto keyValue = split_(s, ' ');
		if(keyValue[0] == "transcript_id") {
			return (split_(keyValue[1], '"')[1] + std::string("_") + chr_name);
		}
	}
	return "";
}

std::string IndexSpacedHashFast::trim_(std::string s) const {
	std::string ret = "";
	int i, j;
	for(i = 0; i < s.size(); i++) {
		if(!std::isspace(s[i])) {
			break;
		}
	}
	for(j = s.size() - 1; j >= 0; j--) {
		if(!std::isspace(s[j])) {
			break;
		}
	}
	for(; i <= j; i++) ret += s[i];
	return ret;
}

std::vector<std::string> IndexSpacedHashFast::split_(std::string s, char c) const {
	std::vector<std::string> v;
    std::string temp = "";
    for(int i = 0; i < s.size(); i++) {
        if(s[i] == c) {
            v.push_back(temp);
            temp = "";
        }
        else
            temp += s[i];
    }
    v.push_back(temp);
	return v;
}

std::string IndexSpacedHashFast::getSequenceName_(const SingleSequence &seq) const {
	std::string name = "";
	for(int i = 0; i < seq.get_header_length() && seq.get_header()[i] != ' '; i++) {
		name += seq.get_header()[i];
	}
	return name;
}

const std::map<std::string, std::vector<std::pair<std::string, char> > >& IndexSpacedHashFast::get_genome_id_to_trans_id() const {
  return genome_id_to_trans_id_;
}

const std::map<std::string, std::vector<std::pair<int64_t, int64_t> > >& IndexSpacedHashFast::get_trans_id_to_exons() const {
  return trans_id_to_exons_;
}

const std::map<std::string, std::vector<std::pair<int64_t, int64_t> > >& IndexSpacedHashFast::get_trans_id_to_regions() const {
  return trans_id_to_regions_;
}

const std::map<std::string, std::pair<std::string, char>>& IndexSpacedHashFast::get_trans_id_to_genome_id() const {
  return trans_id_to_genome_id_;
}

void IndexSpacedHashFast::outputSeq_(char *header, size_t headerLen, const int8_t *seq, size_t seqLen) const {
	std::cout << ">";
	for(int i = 0; i< headerLen; i++)
		std::cout << header[i];
	for(int i = 0; i < seqLen; i++) {
		if(i % 80 == 0) std::cout << "\n";
		std::cout << seq[i];
	}
	std::cout << "\n";
}

void IndexSpacedHashFast::HashGenomeLengths_(const SequenceFile &references, std::map<std::string, int64_t> &rlens) const {
  rlens.clear();
  for (auto& r: references.get_sequences()) {
    rlens[std::string(r->get_header())] = r->get_sequence_length();
    rlens[TrimToFirstSpace(r->get_header())] = r->get_sequence_length();
  }
}

const std::map<std::string, int64_t>& IndexSpacedHashFast::get_genome_id_to_len() const {
  return genome_id_to_len_;
}
