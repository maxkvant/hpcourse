#include "myflowgraph.h"

MyFlowGraph::MyFlowGraph(graph_options opt)
{
    image img;
    images_limit = opt.max_images;
    for(int i = 0; i < opt.max_images; i++)
    {
        img.id = i;
        img.generate(opt.image_w, opt.image_h);
        imgs.push_back(img);
    }

    br = opt.brightness;
    logging = opt.logging;
    log_file = opt.filename;
}

MyFlowGraph::~MyFlowGraph()
{
    for(int i = 0; i < imgs.size(); i++)
        delete[] imgs[i].data;
}

void MyFlowGraph::run()
{
    tbb_graph g;

    tbb::flow::broadcast_node<image> input_img(g);
    tbb::flow::function_node<image, minmax>
            find_min_max(g, images_limit, [](image img)
    {
        minmax res;
        res.img = img;
        res.minmax_ = find_minmax_value(img);
        return res;
    });
    int bri = br;
    tbb::flow::function_node<image, selected_pixels>
            select_elements(g, images_limit, [bri](image img)
    {
        selected_pixels res;
        res.img = img;
        res.pixels = find_elements(img, bri);
        return res;
    });
    tbb::flow::function_node<minmax, selected_pixels>
            select_min_elements(g, images_limit, [](minmax val)
    {
        selected_pixels res;
        res.img = val.img;
        uchar min = val.minmax_.first;
        res.pixels = find_elements(res.img, min);
        return res;
    });
    tbb::flow::function_node<minmax, selected_pixels>
            select_max_elements(g, images_limit, [](minmax val)
    {
        selected_pixels res;
        res.img = val.img;
        uchar max = val.minmax_.second;
        res.pixels = find_elements(res.img, max);
        return res;
    });
    tbb::flow::function_node<selected_pixels, image>
            ext_min(g, images_limit, [](selected_pixels pixs)
    {
        extend_pixels(pixs.img, pixs.pixels);
        return pixs.img;
    });
    tbb::flow::function_node<selected_pixels, image>
            ext_max(g, images_limit, [](selected_pixels pixs)
    {
        extend_pixels(pixs.img, pixs.pixels);
        return pixs.img;
    });
    tbb::flow::function_node<selected_pixels, image>
            ext_br(g, images_limit, [](selected_pixels pixs)
    {
        extend_pixels(pixs.img, pixs.pixels);
        return pixs.img;
    });
    tbb::flow::join_node<std::tuple<image, image, image>, tbb::flow::queueing >
            join(g);
    tbb::flow::function_node<std::tuple<image, image, image>, img_avgbr>
            inverse_and_avg(g, tbb::flow::unlimited, [](std::tuple<image, image, image> tup)
    {
        img_avgbr res;
        res.img = std::get<0>(tup);
        res.avg_br = inverse_and_avgbr(res.img);
        return res;
    });
    tbb::flow::buffer_node<img_avgbr> avg_br(g);

    tbb::flow::make_edge(input_img, find_min_max);
    tbb::flow::make_edge(input_img, select_elements);
    tbb::flow::make_edge(find_min_max, select_min_elements);
    tbb::flow::make_edge(find_min_max, select_max_elements);
    tbb::flow::make_edge(select_min_elements, ext_min);
    tbb::flow::make_edge(select_max_elements, ext_max);
    tbb::flow::make_edge(select_elements, ext_br);
    tbb::flow::make_edge(ext_min, tbb::flow::input_port<0>(join));
    tbb::flow::make_edge(ext_max, tbb::flow::input_port<1>(join));
    tbb::flow::make_edge(ext_br, tbb::flow::input_port<2>(join));
    tbb::flow::make_edge(join, inverse_and_avg);
    tbb::flow::make_edge(inverse_and_avg, avg_br);


    for(int i = 0; i < imgs.size(); i++)
        input_img.try_put(imgs[i]);
    g.wait_for_all();

    if(logging)
        write_avgs_to_file(avg_br);
}

std::pair<uchar, uchar> MyFlowGraph::find_minmax_value(image img)
{
    uchar* min_buffer = new uchar[img.height];
    uchar* max_buffer = new uchar[img.height];
    tbb::parallel_for(size_t(0), size_t(img.height), size_t(1),
        [min_buffer, max_buffer, &img](size_t i) {
            uchar* arr = img.data + i*img.width;
            uchar min = arr[0];
            uchar max = arr[0];
            for(int j = 1; j < img.width; j++)
            {
                if(min > arr[j])
                    min = arr[j];
                if(max < arr[j])
                    max = arr[j];
            }
            min_buffer[i] = min;
            max_buffer[i] = max;
    });
    uchar min = min_buffer[0];
    uchar max = max_buffer[0];
    for(int i = 1; i < img.height; i++)
    {
        if(min > min_buffer[i])
            min = min_buffer[i];
        if(max < max_buffer[i])
            max = max_buffer[i];
    }
    delete[] min_buffer;
    delete[] max_buffer;
    std::pair<uchar, uchar> res;
    res.first = min;
    res.second = max;
    return res;
}

tbb::concurrent_vector<pixel> MyFlowGraph::find_elements(image img, int value)
{
    tbb::concurrent_vector<pixel> result;
    tbb::parallel_for(size_t(0), size_t(img.height), size_t(1),
        [&img, value ,&result](size_t i) {
            pixel p;
            p.h_index = i;
            uchar* arr = img.data + i*img.width;
            for(int j = 0; j < img.width; j++)
            {
                if(arr[j] == value)
                {
                    p.value = value;
                    p.w_index = j;
                    result.push_back(p);
                }
            }
    });
    return result;
}
void MyFlowGraph::extend_pixels(image img, tbb::concurrent_vector<pixel> pixels)
{
    tbb::parallel_for(size_t(0), size_t(pixels.size()), size_t(1),
                      [img, &pixels](size_t i)
    {
        extend_pix(img, pixels[i]);
    });
}

void MyFlowGraph::extend_pix(image img, pixel p)
{
    for(int ii = -1; ii <= 1; ii++)
        for(int jj = -1; jj <= 1; jj++)
        {
            uchar* pix_ptr = img.pix_ptr(p.w_index + jj, p.h_index + ii);
            if(pix_ptr != 0)
                pix_ptr[0] = p.value;
        }
}
double MyFlowGraph::inverse_and_avgbr(image img)
{
    double avg_br = 0;
    double* buffer = new double[img.height];
    tbb::parallel_for(size_t(0), size_t(img.height), size_t(1),
        [buffer, &img](size_t i) {
            buffer[i] = 0;
            for(int j = 0; j < img.width; j++)
            {
                uchar value = img.data[i*img.width + j];
                img.data[i*img.width + j] = 255 - value;
                buffer[i] += value;
            }
            buffer[i] /= img.width;
    });
    for(int i = 0; i < img.height; i++)
        avg_br += buffer[i];
    delete[] buffer;
    return avg_br / img.height;
}

void MyFlowGraph::write_avgs_to_file(tbb::flow::buffer_node<img_avgbr>& node)
{
    std::ofstream of(log_file);
    for(int i = 0; i < imgs.size(); i++)
    {
        img_avgbr res;
        node.try_get(res);
        of << "Avg brightness for image[" << res.img.id << "] = " << res.avg_br << std::endl;
    }
    of.close();
}
