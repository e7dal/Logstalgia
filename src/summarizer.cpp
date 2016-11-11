/*
    Copyright (C) 2008 Andrew Caudwell (acaudwell@gmail.com)

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version
    3 of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "summarizer.h"

#include <algorithm>

#ifdef _WIN32
#include "windows.h"
#define ASSERT(x) if(!(x)) {DebugBreak();}
#else
#define ASSERT(x) assert(x)
#endif

/*

Method:
if leaf:
    - return string
if has children:
    - look at each childs 'word' count and allocate a % of the no_strings to each
      child where the % is  the total word count of the children / the individual count
      of each node (rounding downwards - ie 0.9 -> 0)

      call each child and return the total set of strings from the children.

*/

//SummUnit
SummRow::SummRow() {
    this->words=0;
    this->refs=0;
}

SummRow::SummRow(SummNode* source, bool abbreviated) {
    this->source    = source;
    this->words     = source->words; // TODO: just reference source ??
    this->refs      = source->refs;
    this->abbreviated = abbreviated;

    // should this happen here?
    if(source->parent!=0) prependChar(source->c);
}

void SummRow::buildSummary() {
    expanded.clear();
    source->expand(str, expanded, abbreviated);
}

void SummRow::prependChar(char c) {
    str.insert(0,1,c);
}

//SummNode

SummNode::SummNode(Summarizer* summarizer)
    : summarizer(summarizer), parent(0), c('*'), words(0), refs(0), delimiter(false), delimiters(0) {
}

SummNode::SummNode(Summarizer* summarizer, SummNode* parent, const std::string& str, size_t offset)
    : summarizer(summarizer), parent(parent), c(str[offset]), words(0), refs(0), delimiter(false), delimiters(0) {

    if(summarizer->isDelimiter(c)) {
        delimiter = true;
        delimiters++;
    }

    //if leaf
    if(!addWord(str, ++offset)) {
        words=1;
    }
}

bool SummNode::removeWord(const std::string& str, size_t offset) {

    refs--;

    size_t str_size = str.size() - offset;

    if(!str_size) return false;

    words--;

    bool removed = false;

    for(auto it = children.begin(); it != children.end(); it++) {

        SummNode* child = *it;

        if(child->c == str[offset]) {
            delimiters -= child->delimiters;

            removed = child->removeWord(str,++offset);

            if(child->refs == 0) {
                children.erase(it);
                delete child;
            } else {
                delimiters += child->delimiters;
            }

            break;
        }
    }

    return removed;
}

bool SummNode::addWord(const std::string& str, size_t offset) {

    refs++;

    size_t str_size = str.size() - offset;

    if(!str_size) return false;

    words++;

    for(SummNode* child : children) {
        char c = str[offset];

        if(child->c == c) {

            int old_child_delimeters = child->delimiters;

            if(child->addWord(str, ++offset)) {
                // update delimiter count

                delimiters -= old_child_delimeters;
                delimiters += child->delimiters;

//                if(delimiters > 0) {
//                    debugLog("%d delimiters", delimiters);
//                }

                return true;
            }

            return false;
        }
    }

    SummNode* child = new SummNode(summarizer, this, str, offset);
    children.push_back(child);

    delimiters += child->delimiters;

    return true;
}

void SummNode::debug(int indent) const {

    std::string indentation;
    if(indent>0) indentation.append(indent, ' ');

    debugLog("%snode c=%c refs=%d words=%d delims=%d", indentation.c_str(), c, refs, words, delimiters);

    indent++;

     for(SummNode* child : children) {
        child->debug(indent);
    }
}

std::string SummNode::formatNode(std::string str, int refs) {
    char buff[256];
    snprintf(buff, 256, "%03d %s", refs, str.c_str());

    return std::string(buff);
}

void SummNode::expand(std::string prefix, std::vector<std::string>& vec, bool unsummarized_only) {

    if(children.empty()) {
        vec.push_back(formatNode(prefix, refs));
        return;
    }

    //find top-but-not-root node, expand root node
    std::vector<SummRow>::iterator it;

    for(SummNode* child : children) {
        if(unsummarized_only && !child->unsummarized) continue;

        std::vector<SummRow> strvec;
        child->summarize(strvec, 100);

        for(const SummRow& row : strvec) {
            vec.push_back(formatNode(prefix + row.str, row.refs));
        }
    }
}

void SummNode::summarize(std::vector<SummRow>& output, int max_rows, int depth) {

    ASSERT(max_rows > 0);

    if(children.empty() && parent != 0) {
        output.push_back(SummRow(this));
        return;
    }

    int total_child_words = 0;
    for(SummNode* child : children) {
        total_child_words += child->words;
    }

    // NOTE shouldn't total_child_words == this->words ???

    std::vector<SummNode*> sorted_children = children;

    Summarizer::SortMode sort_mode = summarizer->getSortMode();

    if(sort_mode == Summarizer::DELIMITER_COUNT) {

        // delim sort
        std::sort(sorted_children.begin(), sorted_children.end(),
            [](SummNode*a, SummNode*b) {
                if(b->delimiters == a->delimiters) {
                    // secondary sort by words if same number
                    return b->words < a->words;
                }
                return b->delimiters < a->delimiters;
        });

    } else {
        // word sort
        std::sort(sorted_children.begin(), sorted_children.end(),
            [](SummNode*a, SummNode*b) {
                return b->words < a->words;
        });
    }

    // pre-pass to determine if we expect there to be
    // unsummarized rows

    bool expect_unsummarized = false;

    int child_depth = depth;

    if(delimiter && parent != 0 && parent != summarizer->getRoot()) {
        child_depth++;
    }

    if(max_rows < sorted_children.size()) {
        expect_unsummarized = true;
    } else {
        for(SummNode* child : sorted_children) {

            float percent;

            if(sort_mode == Summarizer::DELIMITER_COUNT && delimiters > 0) {
                percent = (float) child->delimiters / delimiters;

            } else {
                percent = (float) child->words / total_child_words;
            }

            int child_max_rows = (int)(percent * max_rows);

            if(child_max_rows <= 0) {
                expect_unsummarized = true;
                break;
            }
        }
    }

    int available_rows = max_rows;

    if(expect_unsummarized) {
        available_rows--;
    }

    int spare_rows = 0;
    int children_summarized = 0;
    std::deque<SummNode*> unsummarized_children;

    bool allow_partial_abbreviations = true;

    if((delimiter == true || !parent) && depth < summarizer->getAbbreviationDepth()) {
        allow_partial_abbreviations = false;
    }

    // if we have not reached

    for(SummNode* child : sorted_children) {

        float percent;

        // make this a method?
        if(sort_mode == Summarizer::DELIMITER_COUNT && delimiters > 0) {
            percent = (float) child->delimiters / delimiters;

        } else {
            percent = (float) child->words / total_child_words;
        }

        int child_max_rows = (int)(percent * available_rows);

        if(spare_rows > 0) {
            child_max_rows += spare_rows;
            spare_rows = 0;
        }

        if(child_max_rows <= 0) {
            // NOTE: unsummarized indicates row should appear in the mouse over list
            // of an abbreviated row
            child->unsummarized = true;
            unsummarized_children.push_back(child);
            continue;
        }

        std::vector<SummRow> child_output;
        child->summarize(child_output, child_max_rows, child_depth);

        // if this node is a delimiter and we get back abbreviated children
        // force this node to be abbreviated
        if(allow_partial_abbreviations == false) {

            bool has_abbreviated = false;

            for(const SummRow& row : child_output) {

                if(row.abbreviated && !row.source->delimiter) {
                    has_abbreviated = true;
                    break;
                }
            }

            if(has_abbreviated) {
                child_max_rows--;

                if(child_max_rows > 0) {
                    spare_rows += child_max_rows;
                }

                expect_unsummarized = true;
                child->unsummarized = true;
                unsummarized_children.push_back(child);
                continue;
            }
        }

        children_summarized++;

        child->unsummarized = false;

        size_t child_rows = child_output.size();

        ASSERT(child_rows <= child_max_rows);

        for(size_t j=0; j<child_rows; j++) {
            if(parent!=0) child_output[j].prependChar(c);
        }

        output.insert(output.end(), child_output.begin(), child_output.end());

        // if child didnt use all their rows add to spare rows

        if(child_rows < child_max_rows) {
            spare_rows += child_max_rows - child_rows;
        }
    }

    if(children_summarized < sorted_children.size()) {

        ASSERT(expect_unsummarized == true);

        // if only one row ends up being unsummarized
        // we have space to summarize it

        bool abbreviate_self = true;

        if(unsummarized_children.size() == 1) {
            SummNode* child = unsummarized_children.front();

            std::vector<SummRow> child_output;
            child->summarize(child_output, 1, child_depth);

            ASSERT(child_output.size()==1);

            SummRow& child_row = child_output[0];

            // add row if we're allowed to use it
            if(   child_row.abbreviated == false
               || child_row.source->delimiter
               || allow_partial_abbreviations) {
                child->unsummarized = false;
                child_row.prependChar(c);
                output.push_back(child_row);
                abbreviate_self = false;
            }
        }

        if(abbreviate_self) {
            // abbreviated version of this node
            output.push_back(SummRow(this, true));
        }
    }

    ASSERT(output.size() <= max_rows);
}

// SummItem

SummItem::SummItem(Summarizer* summarizer, SummRow unit)
    : summarizer(summarizer) {

    pos  = vec2(-1.0f, -1.0f);
    dest = vec2(-1.0f, -1.0f);

    updateRow(unit);

    destroy=false;
    moving=false;
    departing=false;

    setDest(dest);
}

bool SummItem::isMoving() const {
    return moving;
}

void SummItem::updateRow(const SummRow& unit) {

    this->row = unit;

    vec3 col = summarizer->hasColour() ? summarizer->getColour() : colourHash(unit.str);
    this->colour = vec4(col, 1.0f);

    char buff[1024];

    if(unit.abbreviated) {
        if(summarizer->showCount()) {
            snprintf(buff, 1024, "%03d %s* (%d)", unit.refs, unit.str.c_str(), (int) unit.expanded.size());
        } else {
            snprintf(buff, 1024, "%s* (%d)", unit.str.c_str(), (int) unit.expanded.size());
        }
    } else {
        if(summarizer->showCount()) {
            snprintf(buff, 1024, "%03d %s", unit.refs, unit.str.c_str());
        } else {
            snprintf(buff, 1024, "%s", unit.str.c_str());
        }
    }

    this->displaystr = std::string(buff);
    this->width = summarizer->getFont().getWidth(displaystr);

}

void SummItem::setDest(const vec2& dest) {
    vec2 dist = this->dest - dest;

    if(moving && glm::dot(dist, dist) < 1.0f) return;

    this->oldpos  = pos;
    this->dest    = dest;
    this->elapsed = 0;
    this->eta     = 1.0f;

    destroy   = false;
    moving    = true;
}

void SummItem::setDeparting(bool departing) {
    this->departing = departing;
    if(!departing) destroy = false;
}

void SummItem::setPos(const vec2& pos) {
    this->pos = pos;
}

void SummItem::logic(float dt) {
    if(!moving) return;

    float remaining = eta - elapsed;

    if(remaining > 0.0f) {
        float dist_x = summarizer->getPosX() - pos.x;
        if(dist_x<0.0f) dist_x = -dist_x;

        float alpha = 0.0f;
        if(dist_x<200.0f) {
            alpha = 1.0f - (dist_x/200.0f);
        }

        colour.w = alpha;

        pos = oldpos + (dest-oldpos)*(1.0f - (remaining/eta));
        elapsed+=dt;
    } else {
        pos = dest;
        if(departing) {
            destroy = true;
        }
        departing = false;
        moving    = false;
        colour.w=1.0f;
    }
}

void SummItem::draw(float alpha) {
    FXFont& font = summarizer->getFont();
    font.setColour(vec4(colour.x, colour.y, colour.z, colour.w * alpha));
    font.draw((int)pos.x, (int)pos.y, displaystr.c_str());
}

// Summarizer

Summarizer::Summarizer(FXFont font, int screen_percent, int abbreviation_depth, float refresh_delay, std::string matchstr, std::string title)
    : root(this), matchre(matchstr) {

    pos_x = top_gap = bottom_gap = 0.0f;

    this->screen_percent = screen_percent;
    this->abbreviation_depth = abbreviation_depth;
    this->title = title;
    this->font  = font;

    this->refresh_delay   = refresh_delay;
    this->refresh_elapsed = refresh_delay;

    has_colour = false;
    item_colour= vec3(0.0f);

    showcount=false;
    changed = false;

    incrementf = 0;
}

int Summarizer::getScreenPercent() {
    return screen_percent;
}

void Summarizer::setPosX(float x) {
    if(pos_x == x) return;

    pos_x = x;

    for(SummItem& item : items) {
        item.setPos(vec2(x, item.pos.y));
    }
    refresh_elapsed = 0.0f;
}

bool Summarizer::isAnimating() const {
    for(const SummItem& item : items) {
        if(item.isMoving()) return true;
    }

    return false;
}


float Summarizer::getPosX() const {
    return pos_x;
}

const std::string& Summarizer::getTitle() const {
    return title;
}

const SummNode *Summarizer::getRoot() const {
    return &root;
}

void Summarizer::setSize(int x, float top_gap, float bottom_gap) {
    this->pos_x      = x;
    this->top_gap    = top_gap;
    this->bottom_gap = bottom_gap;

    // TODO: set 'right' explicitly?
    right = (pos_x > (display.width/2)) ? true : false;

    font_gap = font.getMaxHeight() + 4;

    int height = display.height-top_gap-bottom_gap;

    max_strings = (int) ((height)/font_gap);

    // shouldn't this be before max strings ??
    if(!title.empty()) {
        this->top_gap+= font_gap;
    }

    changed = true;
    refresh_elapsed = refresh_delay;
    items.clear();
}

bool Summarizer::mouseOver(const vec2& pos) const {
    if(right && pos.x < pos_x) return false;
    if(pos.y < top_gap || pos.y > (display.height - bottom_gap)) return false;

    return true;
}

const SummItem* Summarizer::itemAtPos(const vec2& pos) {
    for(SummItem& item : items) {
        if(item.departing) continue;

        if(item.pos.y <= pos.y && (item.pos.y+font.getMaxHeight()+4) > pos.y) {
            if(pos.x < item.pos.x || pos.x > item.pos.x + item.width) continue;

            return &item;
        }
    }

    return 0;
}

bool Summarizer::getInfoAtPos(TextArea& textarea, const vec2& pos) {

    if(!mouseOver(pos)) return false;

    float y = pos.y;

    const SummItem* item = itemAtPos(pos);

    if(item != 0) {
        textarea.setText(item->row.expanded);
        textarea.setColour(vec3(item->colour));
        textarea.setPos(pos);

        return true;
    }

    return false;
}

bool Summarizer::setPrefixFilterAtPos(const vec2& pos) {
    return false;
}

void Summarizer::setPrefixFilter(const std::string& prefix_filter) {
    // 1. user clicks on a strings in the summarizer (eg wants to see what images/* contains)
    // 2. summary recalculated to only show strings with that prefix
    // 2. new requests strings are ignored if they dont match filter, existing strings can still be removed

    this->prefix_filter = prefix_filter;
}

const std::string& Summarizer::getPrefixFilter() const {
    return prefix_filter;
}

void Summarizer::setColour(const vec3& col) {
    item_colour = col;
    has_colour = true;
}

bool Summarizer::hasColour() const {
    return has_colour;
}

const vec3& Summarizer::getColour() const {
    return item_colour;
}

bool Summarizer::supportedString(const std::string& str) {
    return matchre.match(str);
}

// string sort with numbers sorted last (reasoning: text is more likely to be interesting)
bool Summarizer::row_sorter(const SummRow& a, const SummRow& b) {

    int ai = atoi(a.str.c_str());
    int bi = atoi(b.str.c_str());
    
    // if only one of a or b is numeric, non numeric value wins
    if((ai==0) != (bi==0)) {
        return ai==0;
    }

    return a.str.compare(b.str) < 0;
}

bool Summarizer::item_sorter(const SummItem& a, const SummItem& b) {
    return row_sorter(a.row, b.row);
}

void Summarizer::summarize() {
    changed = false;

    strings.clear();
    root.summarize(strings, max_strings);

    size_t nostrs = strings.size();

    for(size_t i=0;i<nostrs;i++) {
        strings[i].buildSummary();
    }

    std::sort(strings.begin(), strings.end(), Summarizer::row_sorter);
    
    if(nostrs>1) {
        incrementf  = (((float)display.height-font_gap-top_gap-bottom_gap)/(nostrs-1));
    } else {
        incrementf =0;
    }
}

void Summarizer::getSummary(std::vector<std::string>& summary) const {
    summary.clear();
    for(const SummRow& row : strings) {
        summary.push_back(row.str);
    }
}

void Summarizer::recalc_display() {

    size_t nostrs = strings.size();

    std::vector<bool> strfound;
    strfound.resize(nostrs, false);

    size_t found_count = 0;
    
    //update summItems
    for(SummItem& item : items) {

        int match = -1;

        for(size_t j=0;j<nostrs;j++) {
            const SummRow& summstr = strings[j];

            if(summstr.str.compare(item.row.str) == 0) {
                item.updateRow(summstr);

                match = (int)j;

                strfound[j]= true;
                found_count++;
                
                break;
            }
        }

        item.setDeparting(match == -1 ? true : false);
    }

    //add items for strings not found
    for(size_t i=0;i<nostrs;i++) {
        if(strfound[i]) continue;

        items.push_back(SummItem(this, strings[i]));
        
        //debugLog("added item for unit %s %d", strings[i].str.c_str(), items[items.size()-1].destroy);
    }
    
    // sort items alphabetically
    std::sort(items.begin(), items.end(), Summarizer::item_sorter);

    // set y positions

    int y_index = 0;
        
    for(SummItem& item : items) {
        
        float Y = calcPosY(y_index);

        if(!item.departing) y_index++;

        float startX = right ? display.width + 100 : -200;

        // initialize new item position
        if(item.pos.y<0.0) {
            item.setPos(vec2(startX, Y));
        }
        
        if(item.departing) {
            // return to start
            item.setDest(vec2(startX, Y));
        } else {
            item.setDest(vec2(pos_x, Y));
        }
        
    }
}

void Summarizer::removeString(const std::string& str) {
    root.removeWord(str,0);
    changed = true;
}

float Summarizer::calcPosY(int i) const {
    return top_gap + (incrementf * i) ;
}

int Summarizer::getBestMatchIndex(const std::string& input) const {

    int best_diff = -1;
    int best      = -1;
    int best_size = -1;

    size_t nostrs = strings.size();

    for(size_t i = 0;i < nostrs; i++) {

        size_t strn_size  = strings[i].str.size();
        size_t input_size = input.size();

        int size_diff = strn_size - input_size;

        size_t min_size   = size_diff == 0 ? input_size : std::min ( strn_size, input_size );

        int min_common_diff = abs( strings[i].str.compare(0, min_size, input, 0, min_size) );

        //found
        if(size_diff == 0 && min_common_diff == 0) {
            best = i;
            break;
        }

        if(    best_diff == -1
            || min_common_diff < best_diff
            || (min_common_diff == best_diff //remove this?
               && min_size > best_size
               && strn_size < input_size)) {

            best = i;

            best_diff = min_common_diff;
            best_size = min_size;
        }
    }

    return best;
}

const std::string& Summarizer::getBestMatchStr(const std::string& str) const {
    int pos = getBestMatchIndex(str);

    assert(pos !=- 1);

    return strings[pos].str;
}


float Summarizer::getMiddlePosY(const std::string& str) const {
    return getPosY(str) + (font.getMaxHeight()) / 2;
}

float Summarizer::getPosY(const std::string& str) const {

    int best = getBestMatchIndex(str);

    if(best!=-1) {
        return calcPosY(best);
    }

    return calcPosY(0);
}

void Summarizer::setShowCount(bool showcount) {
    this->showcount = showcount;
}

bool Summarizer::showCount() const {
    return showcount;
}

Summarizer::SortMode Summarizer::getSortMode() const {
    return sort_mode;
}

void Summarizer::setSortMode(SortMode sort_mode) {
    this->sort_mode = sort_mode;
}

void Summarizer::setAbbreviationDepth(int abbreviation_depth) {
    if(abbreviation_depth >= -1) {
        this->abbreviation_depth = abbreviation_depth;
    }
}

int Summarizer::getAbbreviationDepth() const {
    return abbreviation_depth;
}

bool Summarizer::isDelimiter(char c) const {
    for(char delim : delimiters) {
        if(c == delim) return true;
    }

    return false;
}

FXFont& Summarizer::getFont() {
    return font;
}

void Summarizer::addString(const std::string& str) {
    root.addWord(str,0);
    changed = true;
}

void Summarizer::addDelimiter(char c) {
    delimiters.push_back(c);
}

void Summarizer::logic(float dt) {

    if(changed) summarize();

    refresh_elapsed+=dt;
    if(refresh_elapsed>=refresh_delay) {
        recalc_display();
        refresh_elapsed=0;
    }

    //move items
    for(auto it = items.begin();it != items.end();) {
        (*it).logic(dt);

        if((*it).destroy) {
            it = items.erase(it);
        } else {
            it++;
        }
    }
}

void Summarizer::draw(float dt, float alpha) {
   	glEnable(GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);

    if(title.size()) {
        font.setColour(vec4(1.0f, 1.0f, 1.0f, alpha));
        font.draw((int)pos_x, (int)(top_gap - font_gap), title.c_str());
    }

    for(SummItem& item : items) {
        item.draw(alpha);
    }

}
