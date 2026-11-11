/*
 * test_urlparse - channel_parse: every accepted config
 * form, the platform inference rules, case folding,
 * query strings and the reject paths, all offline.
 */

#include "harness.h"

#include "urlparse.h"

static void check_channel(const channel_t *ch, source_t src,
                            const char *slug, const char *url)
{
    CHECK(ch->src == src);
    CHECK_STR_EQ(ch->slug, slug);
    CHECK_STR_EQ(ch->url, url);
}

int main(void)
{
    channel_t ch;

    /* --- the four documented forms --- */

    CHECK(channel_parse("twitch: shroud", &ch));
    check_channel(&ch, SRC_TWITCH, "shroud", "https://twitch.tv/shroud");
    channel_free(&ch);

    CHECK(channel_parse("twitch: https://twitch.tv/twitch", &ch));
    check_channel(&ch, SRC_TWITCH, "twitch", "https://twitch.tv/twitch");
    channel_free(&ch);

    CHECK(channel_parse("youtube: @pokimanel", &ch));
    check_channel(&ch, SRC_YOUTUBE, "_pokimanel",
                  "https://www.youtube.com/@pokimanel");
    channel_free(&ch);

    /* A bare UC… channel id becomes /channel/<id>. */
    CHECK(channel_parse("youtube: UCBc8We6nmyFhtgMuZ5SEP5Q", &ch));
    check_channel(&ch, SRC_YOUTUBE, "channel_UCBc8We6nmyFhtgMuZ5SEP5Q",
                  "https://www.youtube.com/channel/UCBc8We6nmyFhtgMuZ5SEP5Q");
    channel_free(&ch);

    /* --- platform inference, no prefix --- */

    CHECK(channel_parse("https://twitch.tv/shroud", &ch));
    check_channel(&ch, SRC_TWITCH, "shroud", "https://twitch.tv/shroud");
    channel_free(&ch);

    CHECK(channel_parse("https://www.youtube.com/@pokimanel", &ch));
    check_channel(&ch, SRC_YOUTUBE, "_pokimanel",
                  "https://www.youtube.com/@pokimanel");
    channel_free(&ch);

    /* A bare name with no URL at all defaults to Twitch. */
    CHECK(channel_parse("shroud", &ch));
    check_channel(&ch, SRC_TWITCH, "shroud", "https://twitch.tv/shroud");
    channel_free(&ch);

    /* --- case folding --- */

    /* The prefix is case-insensitive, Twitch logins fold to lowercase. */
    CHECK(channel_parse("TWITCH: Shroud", &ch));
    check_channel(&ch, SRC_TWITCH, "shroud", "https://twitch.tv/shroud");
    channel_free(&ch);

    /* YouTube handles keep their case. */
    CHECK(channel_parse("youtube: @Pokimanel", &ch));
    check_channel(&ch, SRC_YOUTUBE, "_Pokimanel",
                  "https://www.youtube.com/@Pokimanel");
    channel_free(&ch);

    /* Trailing whitespace never reaches the slug. */
    CHECK(channel_parse("twitch: shroud   ", &ch));
    check_channel(&ch, SRC_TWITCH, "shroud", "https://twitch.tv/shroud");
    channel_free(&ch);

    /* --- query strings and fragments --- */

    CHECK(channel_parse("twitch: https://twitch.tv/foo?lang=en", &ch));
    check_channel(&ch, SRC_TWITCH, "foo", "https://twitch.tv/foo");
    channel_free(&ch);

    CHECK(channel_parse("twitch: https://twitch.tv/foo#frag", &ch));
    check_channel(&ch, SRC_TWITCH, "foo", "https://twitch.tv/foo");
    channel_free(&ch);

    CHECK(channel_parse("youtube: https://www.youtube.com/@handle?x=1", &ch));
    check_channel(&ch, SRC_YOUTUBE, "_handle",
                  "https://www.youtube.com/@handle");
    channel_free(&ch);

    /* --- malformed input: rejected, owning nothing --- */

    CHECK(!channel_parse("", &ch));
    CHECK(ch.slug == NULL && ch.url == NULL);

    CHECK(!channel_parse("   ", &ch));
    CHECK(ch.slug == NULL && ch.url == NULL);

    CHECK(!channel_parse("twitch:", &ch));
    CHECK(ch.slug == NULL && ch.url == NULL);

    CHECK(!channel_parse("youtube:", &ch));
    CHECK(ch.slug == NULL && ch.url == NULL);

    /* channel_init leaves the same empty state. */
    channel_init(&ch);
    CHECK(ch.slug == NULL && ch.url == NULL);

    /* channel_free returns the channel to that state, and is
     * safe to call again on an already-empty channel. */
    CHECK(channel_parse("twitch: shroud", &ch));
    channel_free(&ch);
    CHECK(ch.slug == NULL && ch.url == NULL);
    channel_free(&ch);

    return harness_exit("test_urlparse");
}
