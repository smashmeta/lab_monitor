#include <QApplication>

#include <boost/program_options.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "cart_window.hpp"
#include "lm/ui/theme.hpp"

namespace po = boost::program_options;

int main(int argc, char** argv) {
    std::uint32_t domain_id = 42;
    std::string topic = "ShoppingCart";

    po::options_description options("Shopping Cart — a DDS publisher to test rules against");
    options.add_options()
        ("help,h", "show this message")
        ("domain-id", po::value<std::uint32_t>(&domain_id)->default_value(42),
         "DDS domain to publish on")
        ("topic", po::value<std::string>(&topic)->default_value("ShoppingCart"), "topic name");

    try {
        po::variables_map values;
        po::store(po::parse_command_line(argc, argv, options), values);
        po::notify(values);
        if (values.count("help") != 0) {
            std::cout << options << "\n\n"
                      << "Verifying a DDS rule with this:\n"
                      << "  1. run this tool, and the client without --offline\n"
                      << "  2. on the server's Templates tab, Add Rule ->\n"
                      << "     \"DDS: value on a topic\", domain " << domain_id << ", topic "
                      << topic << ",\n"
                      << "     path items_.length, equal to 2 -- then Publish\n"
                      << "  3. add items here and watch the Compliance tab follow\n";
            return EXIT_SUCCESS;
        }
    } catch (const std::exception& error) {
        std::cerr << "bad arguments: " << error.what() << "\n" << options << "\n";
        return EXIT_FAILURE;
    }

    QApplication app(argc, argv);
    // The one piece of lab_monitor this fixture borrows, and only so it does
    // not look like a stranger next to the two real windows. Its DDS side is
    // deliberately independent -- see cart_publisher.hpp.
    lm::ui::Theme::apply(app);

    CartWindow window(domain_id, QString::fromStdString(topic));
    window.resize(720, 640);
    window.show();

    return app.exec();
}
