#ifndef CLOUD_H
#define CLOUD_H
#include <opencv2/core/core.hpp>
#include <stdio.h>
#include <vector>

template <class point> class Cloud
{
   public:
      void remove(int index);
      void remove(int, point &p, uchar &d, int &f);
      void remove_last(int);
      void remove_frame(int);
      void add(point, uchar, int);
      void get(int frame,
              std::vector<point>::iterator &p_begin,
              std::vector<point>::iterator &p_end,
              std::vector<uchar>::iterator &d_begin,
              std::vector<uchar>::iterator &d_end);
      void get_points(std::vector<point> &pts);
      void get_descriptors(std::vector<uchar> &dscs);
      void get_frames(std::vector<int> &fs);
      Cloud();
      Cloud(std::vector<point>, std::vector<uchar>);
      ~Cloud();
   private:
      std::vector<point> points;
      std::vector<uchar> descriptors;
      std::vector<int> frames;
};

#endif
